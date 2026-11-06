// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Module side of the Pokemon Sword guest helper (native/helper/sw_guest_helper.S, format-2 load
// plan packages/PokemonSword/dualscreen/guest/<build>/load-plan.json). Battle commands FIGHT with a
// move slot and RUN (phase 1), SWITCH and ITEM (phase 2), committed through the game's own
// battle-client code on the game's own thread (see sw_guest_helper.S's header). No button is ever
// simulated. Phase 2 also adds the battle-alive beat (the client's per-frame update) that keeps the
// battle page live through turn animations and ends it when the battle is gone.
//
// The offsets below are the single source of truth shared with the assembly (which encodes them
// as literal immediates); layout_check at the bottom turns a drift into a compile error.
//
// The helper only runs while the battle command menu waits for input (its hook is the client's
// "wait for the command menu" step), so `wait_beat` advancing IS the "menu is waiting" signal:
// the bottom-screen battle buttons unlock only while it beats.
//
// Header-only and host-independent: Bridge is templated on four offset-addressed primitives
// (the host extension's load_u32/load_u64/store_u32/store_u64: loads acquire, stores release),
// and time is passed in as milliseconds, so the unit test drives it with a fake mailbox and a
// fake clock.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include "sw_strings.h"

namespace sw_helper {

inline constexpr std::uint32_t MailboxSize = 4096;
inline constexpr std::uint32_t EpochOffset = 4092; // host-owned epoch word (mailbox_epoch_offset)

inline constexpr std::uint32_t OffsetModuleMagic = 0x00; // module: 'SWM1'
inline constexpr std::uint32_t OffsetGuestMagic = 0x04;  // guest: 'SWG1' every menu-wait frame
inline constexpr std::uint32_t OffsetWaitBeat = 0x08;    // guest: ++ every menu-wait frame
inline constexpr std::uint32_t OffsetPosInfo = 0x0C;     // guest: position | first_position << 8
inline constexpr std::uint32_t OffsetClient = 0x10;      // guest: battle client object (u64)
inline constexpr std::uint32_t OffsetMon = 0x18;         // guest: client's current mon (u64)
inline constexpr std::uint32_t OffsetReqSeq = 0x20;      // module, release, written last
inline constexpr std::uint32_t OffsetReqOp = 0x24;
inline constexpr std::uint32_t OffsetReqArg = 0x28;      // FIGHT: move slot 0..3
inline constexpr std::uint32_t OffsetReqExpect = 0x2C;   // FIGHT: move id the module showed
inline constexpr std::uint32_t OffsetReqEpoch = 0x30;
inline constexpr std::uint32_t OffsetCancelSeq = 0x34;   // module: "do not run req_seq any more"
// guest, every menu-wait frame (review H2/M4): the battle's kind from its setup param -- bits 0-7
// rule ([main+0x150]: 0 single, 1 double, 2 raid), 8-15 competitor ([setup+0]: 0 wild, 1 trainer,
// 2 institution, 3 link/online), 16-23 comm mode ([setup+0x3B8]: 0 offline), bit 31 = the guest's
// verdict "offline single wild/trainer battle", the only kind direct commands run in.
inline constexpr std::uint32_t OffsetBattleKind = 0x38;
inline constexpr std::uint32_t BattleKindAllowed = 0x80000000u;
inline constexpr std::uint32_t OffsetAckSeq = 0x40;      // guest, release
inline constexpr std::uint32_t OffsetResultCode = 0x44;
inline constexpr std::uint32_t OffsetResultValue = 0x48; // FIGHT: move id committed
inline constexpr std::uint32_t OffsetAckBeat = 0x4C;
// Phase 2: the battle client's per-frame update hook (main+0x7FCE10) beats for the whole battle --
// command menu, turn animations, messages, party/bag screens -- and stops when the battle is gone.
inline constexpr std::uint32_t OffsetAliveBeat = 0x50;   // guest: ++ every battle-client frame
inline constexpr std::uint32_t OffsetAliveClient = 0x58; // guest: that client object (u64)

// HUD hide (2026-10-26): the battle HP plates on the top screen. The helper's third hook (the
// layout work's per-frame draw step, main+0x685A00) acts on the "battle_top_00" layout only:
// while hud_req == 1 (and the module's heartbeat is fresh) it clears the visible bit of each
// plate's N_inout pane, and sets back only what it cleared once hud_req drops (see the
// sw_hud_entry comment in sw_guest_helper.S).
inline constexpr std::uint32_t OffsetHudReq = 0x60;      // module: 1 = hide the plates, 0 = show
inline constexpr std::uint32_t OffsetHudReqBeat = 0x64;  // module: ++ every tick (heartbeat)
inline constexpr std::uint32_t OffsetHudFrame = 0x68;    // guest: ++ every battle_top_00 frame
inline constexpr std::uint32_t OffsetHudState = 0x6C;    // guest: bits 0-3 hidden by the helper,
                                                         //        bits 8-11 plates found this frame
inline constexpr std::uint32_t OffsetHudSeenBeat = 0x70; // guest-private: last heartbeat seen
inline constexpr std::uint32_t OffsetHudStale = 0x74;    // guest-private: frames since it moved
inline constexpr std::uint32_t OffsetHudLayout = 0x78;   // guest: layout the record belongs to (u64)
inline constexpr std::uint32_t OffsetHudSeenBattleSerial = 0xA0; // guest-private: HUD record lifetime
inline constexpr std::uint32_t OffsetBattleSerial = 0xA4; // guest: ++ at native client initialization
inline constexpr std::uint32_t OffsetHudSeenClient = 0xA8; // guest-private: record battle-client (u64)
inline constexpr std::uint32_t OffsetHudPane = 0x80;     // guest: N_inout panes hidden (u64[4])
// Plate slots (hud_state bit / pane index): own, foe, own 2 and foe 2 (doubles).
inline constexpr int HudPlates = 4;
// Native command-menu preview. Source identities and the released menu beat must match before
// any field is used. Gigantamax indices address gwazaname.dat, not the normal move table.
inline constexpr std::uint32_t OffsetDmaxOpportunity = 0xB0;
inline constexpr std::uint32_t OffsetDmaxPreviewBeat = 0xB4;
inline constexpr std::uint32_t OffsetDmaxMoveIds = 0xB8;
inline constexpr std::uint32_t OffsetDmaxPowers = 0xC8;
inline constexpr std::uint32_t OffsetDmaxGmaxIndices = 0xD8;
inline constexpr std::uint32_t OffsetDmaxMon = 0xE8;
inline constexpr std::uint32_t OffsetDmaxClient = 0xF0;
inline constexpr std::uint32_t OffsetRaidTargetMask = 0xF8;
inline constexpr std::uint32_t OffsetBattleKindSerial = 0xFC;
inline constexpr std::uint32_t OffsetDmaxBaseIds = 0x188;
inline constexpr std::uint32_t OffsetHudSeenMode = 0x198;
// Full native companion UI proof covers initial Max Guard and already-active Max Lightning.
// Explicit Max commands remain bounded to the verified offline Raid shape.
inline constexpr bool RaidDirectVerified = true;
inline constexpr bool DynamaxDirectVerified = true;
// A heartbeat that has not moved for this many battle HUD frames means the module is gone: the
// helper shows the plates again (~8 s at 30 fps).
inline constexpr std::uint32_t HudStaleFrames = 240;

// Cursor channel (2026-10-31, bottom -> top cursor sync): moves the NATIVE move-list cursor while
// the client's FIGHT state (main+0x803590) waits at step 2, from the battle client's per-frame
// hook (sw_alive_entry). Nothing is committed; the guest re-checks the client state and every
// vtable (sw_battle_cursor.h chain) and answers with a code (CursorResult).
inline constexpr std::uint32_t OffsetCursorReqSeq = 0x800;    // module, written last
inline constexpr std::uint32_t OffsetCursorReqIndex = 0x804;  // grid index (move row * 2 + 1)
inline constexpr std::uint32_t OffsetCursorReqEpoch = 0x808;
inline constexpr std::uint32_t OffsetCursorReqKind = 0x80C;   // CursorList (2026-11-01)
inline constexpr std::uint32_t OffsetCursorReqList = 0x810;   // u64: the active move list
inline constexpr std::uint32_t OffsetCursorReqView = 0x818;   // u64: the move select view
inline constexpr std::uint32_t OffsetCursorReqClient = 0x820; // u64: the battle client
inline constexpr std::uint32_t OffsetCursorAckSeq = 0x828;    // guest, release
inline constexpr std::uint32_t OffsetCursorResult = 0x82C;    // guest: 0 ok (moved or already there)
enum class CursorResult : std::uint32_t { Ok = 0, Stale = 1, NotWaiting = 2, View = 3, List = 4, Cursor = 5, Range = 6 };
// Which native grid the request moves (each checked against its own client state and view):
// Move = the move list (FIGHT step 2), Command = the command menu (select-action step 5, command
// view +0x630; used where the bottom drives the native menu: doubles / institution battles),
// Target = the doubles target select (FIGHT step 6, target view +0x5D8).
enum class CursorList : std::uint32_t { Move = 0, Command = 1, Target = 2 };

// Module side of the cursor channel: one request in flight, answered within a few frames.
template <class LoadU32, class StoreU32>
class CursorChannel {
public:
    CursorChannel(LoadU32 load32, StoreU32 store32) : load32_(std::move(load32)), store32_(std::move(store32)) {}
    bool Initialize() {
        std::uint32_t ack = 0;
        if (!load32_(OffsetCursorAckSeq, &ack)) return false;
        next_seq_ = ack;
        pending_ = false;
        return store32_(OffsetCursorReqSeq, ack);
    }
    // row 0..3 of the move list; list/view/client from sw_battle_cursor::Sample this tick.
    bool Stage(int row, std::uint64_t list, std::uint64_t view, std::uint64_t client, std::int64_t now_ms) {
        if (row < 0 || row > 3) return false;
        return StageIndex(CursorList::Move, static_cast<std::uint32_t>(row * 2 + 1), list, view, client, now_ms);
    }
    // Any grid: the raw grid index (command cell index; target = TargetGridFromSlot).
    bool StageIndex(CursorList kind, std::uint32_t index, std::uint64_t list, std::uint64_t view,
                    std::uint64_t client, std::int64_t now_ms) {
        std::uint32_t epoch = 0;
        if (pending_ || index > 255 || !list || !view || !client ||
            !load32_(EpochOffset, &epoch) || !epoch) return false;
        std::uint32_t seq = next_seq_ + 1;
        if (seq == 0) seq = 1;
        const auto store64 = [&](std::uint32_t off, std::uint64_t v) {
            return store32_(off, static_cast<std::uint32_t>(v)) && store32_(off + 4, static_cast<std::uint32_t>(v >> 32));
        };
        bool ok = store32_(OffsetCursorReqIndex, index);
        ok = store32_(OffsetCursorReqKind, static_cast<std::uint32_t>(kind)) && ok;
        ok = store32_(OffsetCursorReqEpoch, epoch) && ok;
        ok = store64(OffsetCursorReqList, list) && ok;
        ok = store64(OffsetCursorReqView, view) && ok;
        ok = store64(OffsetCursorReqClient, client) && ok;
        ok = ok && store32_(OffsetCursorReqSeq, seq); // last
        if (!ok) return false;
        next_seq_ = seq;
        pending_ = true;
        staged_at_ = now_ms;
        return true;
    }
    // True once when the pending request was answered (result in *code) or timed out (*code =
    // Stale). A late guest answer after a timeout is harmless: the guest only ever acts while the
    // FIGHT state waits, after re-checking every pointer.
    bool Poll(std::int64_t now_ms, CursorResult* code) {
        if (!pending_) return false;
        std::uint32_t ack = 0, result = 0;
        if (load32_(OffsetCursorAckSeq, &ack) && ack == next_seq_ && load32_(OffsetCursorResult, &result)) {
            pending_ = false;
            *code = static_cast<CursorResult>(result);
            return true;
        }
        if (now_ms - staged_at_ > 1500) {
            pending_ = false;
            *code = CursorResult::Stale;
            return true;
        }
        return false;
    }
    bool pending() const { return pending_; }

private:
    LoadU32 load32_;
    StoreU32 store32_;
    std::uint32_t next_seq_ = 0;
    bool pending_ = false;
    std::int64_t staged_at_ = 0;
};

// Field channel (2026-10-26): the guest's fourth hook, the field process's per-frame update
// (main+0xDBD850, sw_field_entry). Its beat means the field process updated, not that the player
// has control: live verification 2026-10-28 found it also updates behind wild battles. Field
// actions additionally require field_free, and the module's non-battle phase gate. Its channel is
// separate from the battle one (own seq/ack), so neither hook can ever run the other's request.
inline constexpr std::uint32_t OffsetFieldBeat = 0x100;       // guest: ++ every field frame
// guest, every field frame: 1 = the player has control (no field task -- event script / NPC
// dialogue / cutscene / warp / X-menu launcher -- and no event about to start), the game's own
// condition for dispatching field input (review H1; sw_guest_helper.S field_free).
inline constexpr std::uint32_t OffsetFieldFree = 0x104;
inline constexpr std::uint32_t OffsetFieldReqSeq = 0x110;     // module, release, written last
inline constexpr std::uint32_t OffsetFieldReqOp = 0x114;
inline constexpr std::uint32_t OffsetFieldReqArg = 0x118;     // item id
inline constexpr std::uint32_t OffsetFieldReqExpect = 0x11C;  // medicine: party slot
inline constexpr std::uint32_t OffsetFieldReqEpoch = 0x120;
inline constexpr std::uint32_t OffsetFieldCancelSeq = 0x124;
inline constexpr std::uint32_t OffsetFieldZone = 0x128;       // FLY: zone hash (u64)
inline constexpr std::uint32_t OffsetFieldLanding = 0x130;    // FLY: landing hash (u64)
inline constexpr std::uint32_t OffsetFieldExpectEc = 0x138;   // MEDICINE: the target slot's PK8 encryption
                                                              // constant the module showed (review M1)
inline constexpr std::uint32_t OffsetFieldMove = 0x13C; // MEDICINE: selected move slot0..3 (PP items)
inline constexpr std::uint32_t OffsetFieldAckSeq = 0x140;     // guest, release
inline constexpr std::uint32_t OffsetFieldResultCode = 0x144;
inline constexpr std::uint32_t OffsetFieldResultValue = 0x148;
inline constexpr std::uint32_t OffsetFieldAckBeat = 0x14C;
inline constexpr std::uint32_t OffsetFlyQueryCount = 0x150;   // module: pairs to check
inline constexpr std::uint32_t OffsetFlyQueryMask = 0x158;    // guest: bit i = destination i available (u64)
inline constexpr std::uint32_t OffsetFlyQueryState = 0x160;   // guest: 0 flying allowed here, 1/2 refused
inline constexpr std::uint32_t OffsetBikeUsable = 0x168; // FLY_QUERY: native CanBike verdict
inline constexpr std::uint32_t OffsetFlyQueryPairs = 0x200;   // module: {zone u64, landing u64}[count]
inline constexpr std::uint32_t FlyQueryMax = 48;

// Field ops (mailbox 0x114).
inline constexpr std::uint32_t OffsetFieldProgressionSeq = 0x170;
inline constexpr std::uint32_t OffsetMapQueryZone = 0x178;
inline constexpr std::uint32_t OffsetMapQueryParent = 0x180;
enum class FieldOp : std::uint32_t { None = 0, Medicine = 1, Repel = 2, Fly = 3, FlyQuery = 4, Bike = 5, Progression = 6,
                                         PartySwap = 7 }; // arg from, expect to, ec 0x138 / 0x13C
// Desktop native EXP/move-learning/stone/count and complete evolution return are verified.
inline constexpr bool FieldProgressionVerified = true;
inline bool IsFieldProgression(int item) {
    return item == 50 || (item >= 1124 && item <= 1128) || (item >= 80 && item <= 85) ||
           (item >= 107 && item <= 109) || item == 849;
}

// The field items the channel uses (the guest re-checks the same set). All route through the
// Bag's own CanUse -> SubItem -> Apply under the party/item locks; PP items pass the explicitly
// selected move slot. Candy/evolution items use the separate persistent progression channel.
inline constexpr std::uint64_t kFieldMedicineMask =
    (((1ull << 34) - 1) & ~((1ull << 17) - 1)) | // HP/status medicine, Revive and Max Revive
    (1ull << 38) | (1ull << 39) | (1ull << 40) | (1ull << 41) | // Ether / Elixir variants
    (1ull << 51) | (1ull << 53); // PP Up / PP Max
inline bool IsFieldMedicine(int item) { return item > 0 && item < 64 && ((kFieldMedicineMask >> item) & 1); }
inline bool IsFieldPpSingle(int item) { return item == 38 || item == 39 || item == 51 || item == 53; }
inline bool IsFieldPpAll(int item) { return item == 40 || item == 41; }
inline bool IsRepel(int item) { return item == 76 || item == 77 || item == 79; }

inline constexpr std::uint32_t ExpectRevive = 0x100;
inline constexpr std::uint32_t ModuleMagic = 0x314D5753; // "SWM1" little-endian
inline constexpr std::uint32_t GuestMagic = 0x31475753;  // "SWG1"

// Switch: arg = target party member's pokeID (0..5, the bpp array index). Item: arg = item id,
// expect = target pokeID (medicine) | ExpectRevive (the item restores a fainted member) -- balls
// ignore it.
enum class Op : std::uint32_t { None = 0, Fight = 1, Run = 2, Switch = 3, Item = 4, MaxFight = 5,
                              OpenBag = 6, OpenPokemon = 7,
                              // arg = native command (1 FIGHT, 2 BAG): the guest writes the command
                              // UI's decided command, so the game itself opens the move list / bag.
                              Decide = 8 };
inline constexpr std::uint32_t DecideFight = 1, DecideBag = 2;   // live dev24: 3 opened the Pokemon list

// Mirrors sw_guest_helper.S's refuse_* codes.
enum class Result : std::uint32_t {
    Ok = 0,
    NotConfigured = 1,
    BadOp = 2,
    StaleEpoch = 3,
    Cancelled = 4,
    NotWaiting = 5,
    BadArg = 6,
    NoMove = 7,
    Mismatch = 8,
    NoPp = 9,
    Blocked = 10,   // the game's own "move cannot be selected" check (Disable/Taunt/Choice/...)
    NotFirst = 11,
    Trapped = 12,     // the game's own switch-out check (the party screen's "can't switch" flag)
    Fainted = 13,     // the game's own IsDead on the chosen member
    Active = 14,      // the chosen member is already battling
    NotInParty = 15,  // pokeID not in this client's battle party
    BallRefused = 16, // the game's own ball-throw check (trainer battle, two foes, ...)
    NoEffect = 17,    // the game's own medicine check (full HP, fainted target, ...)
    ItemsBlocked = 18,// the game's own "items can't be used in this battle" checks
    // Field channel (2026-10-26, the field process hook): see FieldBridge below.
    FlyNotHere = 20,  // StateCheckFly: the player/world check ("can't call a Flying Taxi to where you are now")
    FlyNotNow = 21,   // StateCheckFly: the event flag that forbids flying
    FlyLocked = 22,   // the destination's own unlock flag is not set ("You can't fly there yet.")
    BadDest = 23,     // the landing lookup found no such destination
    RepelActive = 24, // the game's own "a Repel is still in effect" check
    NotUsable = 25,   // not an item the field channel uses (kFieldMedicineMask, Repel/Super/Max Repel)
    NoItem = 26,      // the bag holds none of it, or SubItem removed nothing (review M2)
    TargetChanged = 27, // the party slot no longer holds the member the tile showed (review M1)
    NotFree = 28,     // the player does not have control (a script/event/dialogue runs, review H1)
    BikeBlocked = 29, // native Plus-button CanBike predicate refused
    DynamaxUnavailable = 30,
    TargetUnsupported = 31,
    ProgressionInterrupted = 32,
    BattleType = 19,  // battle commands: not an offline single battle (review H2/M4)
};

inline const char* ResultText(Result r) {
    using sw_str::Id;
    using sw_str::S;
    switch (r) {
    case Result::Ok: return "";
    case Result::Cancelled: return S(Id::kResCancelled);
    case Result::StaleEpoch: return S(Id::kResExpired);
    case Result::NotWaiting: return S(Id::kResNotWaiting);
    case Result::NoPp: return S(Id::kResNoPp);
    case Result::Blocked: return S(Id::kResBlocked);
    case Result::Mismatch: return S(Id::kResMismatch);
    case Result::NotFirst: return S(Id::kResNotFirst);
    case Result::Trapped: return S(Id::kResTrapped);
    case Result::Fainted: return S(Id::kResFainted);
    case Result::Active: return S(Id::kResActive);
    case Result::NotInParty: return S(Id::kResNotInParty);
    case Result::BallRefused: return S(Id::kResBallRefused);
    case Result::NoEffect: return S(Id::kResNoEffect);
    case Result::ItemsBlocked: return S(Id::kResItemsBlocked);
    // Field refusals: the game's own message text (bag / townmap message files; strings.json).
    case Result::FlyNotHere:
    case Result::FlyNotNow: return S(Id::kResFlyNotNow);
    case Result::FlyLocked: return S(Id::kResFlyLocked);
    case Result::BadDest: return S(Id::kResBadDest);
    case Result::RepelActive: return S(Id::kResRepel);
    case Result::NotUsable: return S(Id::kResNotUsable);
    case Result::NoItem: return S(Id::kResNoItem);
    case Result::TargetChanged: return S(Id::kResTargetChanged);
    case Result::NotFree: return S(Id::kResNotFree);
    case Result::BikeBlocked: return S(Id::kResBikeBlocked);
    case Result::BattleType: return S(Id::kResBattleType);
    case Result::DynamaxUnavailable: return S(Id::kResNoDynamax);
    case Result::TargetUnsupported: return S(Id::kResTargetUnsupported);
    case Result::ProgressionInterrupted: return S(Id::kResProgression);
    default: return S(Id::kResRefused);
    }
}

// What the module's own reader knows this tick about the active battler (sw_battle.h pass A).
struct BattleView {
    bool in_battle = false;
    bool own_ok = false;
    bool foe_ok = false;
    bool foe_fainted = false;
    bool max_active = false;
    std::array<std::uint16_t, 4> move_id{};
    std::array<std::uint8_t, 4> pp{};
    // Phase 2: the own party's battle params (bpp array index = pokeID 0..5), live.
    int active_pid = -1;
    struct Member {
        bool ok = false;      // decoded and valid this tick
        bool fainted = false; // current HP 0
        std::uint16_t hp = 0, max_hp = 0;
        bool has_status = false; // non-volatile status (live party), for Full Restore
    };
    std::array<Member, 6> members{};
};

// Battle bag items the bottom screen offers (Bag tab), in display order. Medicine needs a target
// (the active battler or a picked bench member); balls do not.
struct BattleItem {
    std::uint16_t id;
    bool ball;
    bool revive; // targets a fainted member only
};
inline constexpr std::array<BattleItem, 9> kBattleItems{{
    {17, false, false},  // Potion
    {26, false, false},  // Super Potion
    {25, false, false},  // Hyper Potion
    {24, false, false},  // Max Potion
    {23, false, false},  // Full Restore
    {28, false, true},   // Revive
    {4, true, false},    // Poke Ball
    {3, true, false},    // Great Ball
    {2, true, false},    // Ultra Ball
}};
inline const BattleItem* FindBattleItem(std::uint16_t id) {
    for (const auto& it : kBattleItems)
        if (it.id == id) return &it;
    return nullptr;
}

// Synchronous refusal reasons at tap time (each maps to the `refused` haptic).
enum class Refusal {
    None, NoHelper, NotInBattle, MenuNotWaiting, Busy, BadSlot, NoMove, NoPp, NotFirst,
    NoSelection, NotMember, ActiveMember, FaintedMember, NoItem, NotBattleItem, TargetNotFainted,
    NoEffect, BattleType,
};

inline const char* RefusalText(Refusal r) {
    using sw_str::Id;
    using sw_str::S;
    switch (r) {
    case Refusal::NoSelection: return S(Id::kRefPickPokemon);
    case Refusal::ActiveMember: return S(Id::kResActive);
    case Refusal::FaintedMember: return S(Id::kResFainted);
    case Refusal::NoItem: return S(Id::kRefPickItem);
    case Refusal::TargetNotFainted: return S(Id::kResNoEffect);
    case Refusal::NoEffect: return S(Id::kResNoEffect);
    case Refusal::BattleType: return S(Id::kResBattleType);
    default: return "";
    }
}

struct Status {
    bool helper_present = false; // guest magic seen and epoch valid
    bool menu_waiting = false;   // wait_beat advanced within the adaptive window
    bool busy = false;           // a request is in flight
    std::uint32_t pos_info = 0;
    // Phase 2: the battle client's per-frame beat advanced within AliveWindowMs -- true through the
    // whole battle (turn animations, messages, the native party/bag screens), false once the
    // battle is gone. `alive_client` is the client object that beat (0 when not alive).
    bool battle_alive = false;
    std::uint64_t alive_client = 0;
    // Fresh native battle kind supports offline single wild/trainer commands and the verified
    // offline wild Raid shape's bounded boss/self FIGHT actions. Doubles, Dynamax Adventures,
    // institution and link/online formats keep commands locked; the guest repeats these gates.
    std::uint32_t battle_kind = 0;
    std::uint32_t battle_serial = 0;
    bool offline_single = false;
    bool kind_current = false;
    bool offline_raid = false;
    struct Preview {
        bool valid = false;
        bool eligible = false;
        bool active = false;
        std::uint8_t remaining = 0, total = 0;
        std::array<std::uint32_t, 4> ids{}, powers{}, base_ids{};
        std::array<std::int32_t, 4> gmax{{-1, -1, -1, -1}};
        std::uint64_t mon = 0, client = 0;
        std::uint32_t target_mask = 0;
    } preview;
};

inline bool FirstPosition(std::uint32_t pos_info) {
    return (pos_info & 0xFF) == ((pos_info >> 8) & 0xFF);
}

inline bool RaidTargetMatches(const Status::Preview& p, const BattleView& v, int slot, bool max) {
    if (!p.valid || slot < 0 || slot > 3) return false;
    const auto k = static_cast<unsigned>(slot);
    return p.base_ids[k] == v.move_id[k] && p.base_ids[k] != 0 &&
           ((p.target_mask >> (k + (max ? 4 : 0))) & 1) != 0;
}

inline Refusal CheckFight(const Status& s, const BattleView& v, int slot, bool max = false) {
    if (!s.helper_present) return Refusal::NoHelper;
    if (!v.in_battle || !v.own_ok) return Refusal::NotInBattle;
    if (!s.menu_waiting) return Refusal::MenuNotWaiting;
    if (!s.offline_single && !(RaidDirectVerified && s.offline_raid)) return Refusal::BattleType;
    if (s.busy) return Refusal::Busy;
    if (slot < 0 || slot > 3) return Refusal::BadSlot;
    if (v.move_id[static_cast<std::size_t>(slot)] == 0) return Refusal::NoMove;
    if (v.pp[static_cast<std::size_t>(slot)] == 0) return Refusal::NoPp;
    if (max && (!DynamaxDirectVerified || !s.offline_raid || !s.preview.valid ||
                !(s.preview.eligible || s.preview.active))) return Refusal::BattleType;
    if (s.offline_raid && !RaidTargetMatches(s.preview, v, slot, max)) return Refusal::BattleType;
    return Refusal::None;
}

inline Refusal CheckRun(const Status& s, const BattleView& v) {
    if (!s.helper_present) return Refusal::NoHelper;
    if (!v.in_battle) return Refusal::NotInBattle;
    if (!s.menu_waiting) return Refusal::MenuNotWaiting;
    if (!s.offline_single) return Refusal::BattleType;
    if (s.busy) return Refusal::Busy;
    if (!FirstPosition(s.pos_info)) return Refusal::NotFirst;
    return Refusal::None;
}

// Enter the native picker without choosing an item/member or spending the turn.
inline Refusal CheckOpenMenu(const Status& s, const BattleView& v) {
    if (!s.helper_present) return Refusal::NoHelper;
    if (!v.in_battle || !v.own_ok) return Refusal::NotInBattle;
    if (!s.menu_waiting) return Refusal::MenuNotWaiting;
    if (!s.offline_single) return Refusal::BattleType;
    if (s.busy) return Refusal::Busy;
    if (!FirstPosition(s.pos_info)) return Refusal::NotFirst;
    return Refusal::None;
}

// Native item IDs from the shipped itemname table. This is the complete SwSh ball
// set; Park Ball500 is deliberately excluded. The guest repeats its native item
// parameter classification and BallCheck before committing the selected item.
inline bool IsBattleBall(int id) {
    return (id >= 1 && id <= 16) || (id >= 492 && id <= 499) || id == 576 || id == 851;
}

inline Refusal CheckBall(const Status& s, const BattleView& v, int id, int count) {
    const auto menu = CheckOpenMenu(s, v);
    if (menu != Refusal::None) return menu;
    if ((s.battle_kind & 0x00FFFFFFu) != 0) return Refusal::BattleType;
    if (!v.foe_ok) return Refusal::NotInBattle;
    if (v.foe_fainted) return Refusal::NoEffect;
    if (!IsBattleBall(id)) return Refusal::NotBattleItem;
    if (count <= 0) return Refusal::NoItem;
    return Refusal::None;
}

// SWITCH to party member `pid` (bpp index). Offline singles only, so the one active battler is
// battle-party index 0 / the client's mon -- the guest refuses both (review H2), plus its own
// trapped/fainted checks.
inline Refusal CheckSwitch(const Status& s, const BattleView& v, int pid) {
    if (!s.helper_present) return Refusal::NoHelper;
    if (!v.in_battle || !v.own_ok) return Refusal::NotInBattle;
    if (!s.menu_waiting) return Refusal::MenuNotWaiting;
    if (!s.offline_single) return Refusal::BattleType;
    if (s.busy) return Refusal::Busy;
    if (pid < 0) return Refusal::NoSelection;
    if (pid > 5 || !v.members[static_cast<std::size_t>(pid)].ok) return Refusal::NotMember;
    if (pid == v.active_pid) return Refusal::ActiveMember;
    if (v.members[static_cast<std::size_t>(pid)].fainted) return Refusal::FaintedMember;
    return Refusal::None;
}

// ITEM `item_id` (count from the live bag) on member `target_pid` (medicine only). The game's own
// medicine/ball checks run in the helper.
inline Refusal CheckItem(const Status& s, const BattleView& v, int item_id, int count, int target_pid) {
    if (!s.helper_present) return Refusal::NoHelper;
    if (!v.in_battle || !v.own_ok) return Refusal::NotInBattle;
    if (!s.menu_waiting) return Refusal::MenuNotWaiting;
    if (!s.offline_single) return Refusal::BattleType;
    if (s.busy) return Refusal::Busy;
    if (item_id <= 0) return Refusal::NoItem;
    const BattleItem* it = FindBattleItem(static_cast<std::uint16_t>(item_id));
    if (!it) return Refusal::NotBattleItem;
    if (count <= 0) return Refusal::NoItem;
    if (!FirstPosition(s.pos_info)) return Refusal::NotFirst;
    if (it->ball) return Refusal::None;
    if (target_pid < 0 || target_pid > 5 || !v.members[static_cast<std::size_t>(target_pid)].ok)
        return Refusal::NotMember;
    // The battle bag screen's own "It won't have any effect." cases for the offered medicine (the
    // game's per-target check is server-side code with no standalone entry -- REA 2026-10-26 --
    // so the HP/status half is checked here from the same live bpp; the helper re-checks
    // fainted/not-fainted with the game's IsDead, and the server is the final authority: a
    // no-effect item is not consumed, but the turn would be spent, hence refusing up front).
    const auto& m = v.members[static_cast<std::size_t>(target_pid)];
    if (it->revive) return m.fainted ? Refusal::None : Refusal::TargetNotFainted;
    if (m.fainted) return Refusal::NoEffect;
    const bool full_hp = m.hp >= m.max_hp;
    if (it->id == 23 /* Full Restore */) return full_hp && !m.has_status ? Refusal::NoEffect : Refusal::None;
    return full_hp ? Refusal::NoEffect : Refusal::None;
}

struct Outcome {
    bool done = false;
    bool answered = false; // false: timed out without any guest answer
    Op op = Op::None;
    Result code = Result::NotConfigured;
    std::uint32_t value = 0;
};

template <class LoadU32, class StoreU32>
class Bridge {
public:
    Bridge(LoadU32 load32, StoreU32 store32) : load32_(std::move(load32)), store32_(std::move(store32)) {}

    // Once after configure(): claim the mailbox for this module build. Never touches the epoch
    // word (the host owns it) nor anything the guest writes.
    bool Initialize() {
        std::uint32_t ack = 0;
        load32_(OffsetAckSeq, &ack);
        next_seq_ = ack; // never reuse a sequence number the guest already answered
        bool ok = store32_(OffsetCancelSeq, 0);
        ok = store32_(OffsetReqSeq, ack) && ok; // nothing pending
        ok = store32_(OffsetModuleMagic, ModuleMagic) && ok;
        return ok;
    }

    // Adaptive windows (BOTW SelectionBridge precedent): the guest beats once per game frame
    // while the menu waits; at ~30 fps that is ~33 ms, at 8 fps ~125 ms.
    std::int64_t BeatWindowMs() const { return std::clamp<std::int64_t>(frame_ms_ * 3 / 2, 100, 400); }
    std::int64_t AckWindowMs() const { return std::clamp<std::int64_t>(frame_ms_ * 2 + 50, 250, 800); }
    // The alive beat comes every battle frame; a battle is over once it has been silent for ~6
    // frames (clamped 300-1500 ms). A host pause/invalidate (epoch change) refreshes it once, so a
    // short pause mid-battle does not read as the battle ending.
    std::int64_t AliveWindowMs() const {
        const std::int64_t f = alive_frame_ms_ ? alive_frame_ms_ : frame_ms_;
        return std::clamp<std::int64_t>(f * 6, 300, 1500);
    }
    std::int64_t FrameMs() const { return frame_ms_; }

    void Update(std::int64_t now_ms) {
        std::uint32_t magic = 0, epoch = 0, beat = 0, pos = 0;
        const bool magic_ok = load32_(OffsetGuestMagic, &magic) && magic == GuestMagic;
        const bool epoch_ok = load32_(EpochOffset, &epoch) && epoch != 0;
        if (epoch_ok && epoch != epoch_) {
            epoch_ = epoch;
            beat_at_ = -1; // a pause/invalidate: the menu must be seen waiting again
            // Only a battle that was alive at the pause is carried over it: refreshing a stale
            // beat from an earlier battle would read as a new battle in the field (one phantom
            // battle page per pause).
            if (alive_at_ >= 0 && status_.battle_alive) alive_at_ = now_ms;
        }
        std::uint32_t alive = 0;
        if (load32_(OffsetAliveBeat, &alive) && alive != alive_) {
            if (alive_at_ >= 0) {
                const std::int64_t d = now_ms - alive_at_;
                if (d > 0 && d <= 1000) alive_frame_ms_ = alive_frame_ms_ ? (alive_frame_ms_ * 3 + d) / 4 : d;
            }
            if (alive_seen_) alive_at_ = now_ms; // the first value read is history, not a beat
            alive_ = alive;
            alive_seen_ = true;
        }
        if (load32_(OffsetWaitBeat, &beat) && beat != beat_) {
            if (beat_at_ >= 0) {
                const std::int64_t d = now_ms - beat_at_;
                if (d > 0 && d <= 1000) frame_ms_ = frame_ms_ ? (frame_ms_ * 3 + d) / 4 : d;
            }
            if (beat_seen_) beat_at_ = now_ms; // the first value read is history, not a beat
            beat_ = beat;
            beat_seen_ = true;
        }
        load32_(OffsetPosInfo, &pos);
        std::uint32_t kind = 0;
        load32_(OffsetBattleKind, &kind);
        status_.helper_present = magic_ok && epoch_ok;
        status_.pos_info = pos;
        status_.battle_kind = kind;
        status_.offline_single = (kind & BattleKindAllowed) != 0;
        std::uint32_t serial = 0, kind_serial = 0;
        status_.kind_current = load32_(OffsetBattleSerial, &serial) && serial != 0 &&
                               load32_(OffsetBattleKindSerial, &kind_serial) && kind_serial == serial;
        status_.battle_serial = serial;
        PollPending(now_ms); // may close the menu (a commit acknowledged this tick)
        status_.menu_waiting = status_.helper_present && beat_at_ >= 0 && now_ms - beat_at_ <= BeatWindowMs();
        status_.busy = pending_;
        status_.battle_alive = status_.helper_present && alive_at_ >= 0 && now_ms - alive_at_ <= AliveWindowMs();
        status_.alive_client = 0;
        if (status_.battle_alive) {
            std::uint32_t lo = 0, hi = 0, lo2 = 0;
            if (load32_(OffsetAliveClient, &lo) && load32_(OffsetAliveClient + 4, &hi) &&
                load32_(OffsetAliveClient, &lo2) && lo == lo2)
                status_.alive_client = (static_cast<std::uint64_t>(hi) << 32) | lo;
        }
        status_.preview = {};
        status_.offline_raid = false;
        std::uint32_t preview_beat = 0;
        if (status_.kind_current && status_.menu_waiting &&
            load32_(OffsetDmaxPreviewBeat, &preview_beat) && preview_beat != 0 && preview_beat == beat_) {
            Status::Preview pv;
            const auto load64 = [&](std::uint32_t off, std::uint64_t& value) {
                std::uint32_t lo = 0, hi = 0, check = 0;
                if (!load32_(off, &lo) || !load32_(off + 4, &hi) || !load32_(off, &check) || lo != check)
                    return false;
                value = (static_cast<std::uint64_t>(hi) << 32) | lo;
                return true;
            };
            std::uint32_t opportunity = 0, end_beat = 0;
            std::uint64_t current_client = 0, current_mon = 0;
            bool ok = load32_(OffsetDmaxOpportunity, &opportunity) &&
                      load64(OffsetDmaxMon, pv.mon) && load64(OffsetDmaxClient, pv.client) &&
                      load64(OffsetClient, current_client) && load64(OffsetMon, current_mon) &&
                      pv.mon != 0 && pv.mon == current_mon && pv.client != 0 &&
                      pv.client == current_client && pv.client == status_.alive_client &&
                      load32_(OffsetRaidTargetMask, &pv.target_mask);
            pv.eligible = (opportunity & 0xff) == 1;
            pv.active = ((opportunity >> 24) & 0xff) == 1;
            pv.remaining = (opportunity >> 8) & 0xff;
            pv.total = (opportunity >> 16) & 0xff;
            ok = ok && (opportunity & 0xff) <= 1 && (opportunity >> 24) <= 1 &&
                 pv.remaining <= pv.total && pv.total <= 4;
            for (std::uint32_t k = 0; k < 4 && ok; ++k) {
                std::uint32_t gmax = 0;
                ok = load32_(OffsetDmaxMoveIds + k * 4, &pv.ids[k]) &&
                     load32_(OffsetDmaxPowers + k * 4, &pv.powers[k]) &&
                     load32_(OffsetDmaxGmaxIndices + k * 4, &gmax) &&
                     load32_(OffsetDmaxBaseIds + k * 4, &pv.base_ids[k]);
                pv.gmax[k] = static_cast<std::int32_t>(gmax);
                ok = ok && pv.ids[k] <= 826 && pv.base_ids[k] <= 826 && pv.powers[k] <= 250 &&
                     pv.gmax[k] >= -1 && pv.gmax[k] <= 32;
            }
            if (ok && load32_(OffsetDmaxPreviewBeat, &end_beat) && end_beat == preview_beat) {
                pv.valid = true;
                status_.preview = pv;
                status_.offline_raid = kind == 2; // guest publishes preview only for submode5/local0
            }
        }
    }

    const Status& status() const { return status_; }

    // Stage one request. Callers run CheckFight/CheckRun first; this re-checks only what the
    // mailbox itself needs (helper present, nothing in flight, non-zero epoch).
    bool Stage(Op op, std::uint32_t arg, std::uint32_t expect, std::int64_t now_ms) {
        if (pending_ || !status_.helper_present || epoch_ == 0 || op == Op::None) return false;
        std::uint32_t seq = next_seq_ + 1;
        if (seq == 0) seq = 1;
        bool ok = store32_(OffsetReqOp, static_cast<std::uint32_t>(op));
        ok = store32_(OffsetReqArg, arg) && ok;
        ok = store32_(OffsetReqExpect, expect) && ok;
        ok = store32_(OffsetReqEpoch, epoch_) && ok;
        ok = store32_(OffsetReqSeq, seq) && ok; // release, last
        if (!ok) return false;
        next_seq_ = seq;
        pending_ = true;
        pending_op_ = op;
        staged_at_ = now_ms;
        cancel_at_ = -1;
        status_.busy = true;
        return true;
    }

    // Module teardown (review L1): cancel anything staged, then withdraw the module magic -- the
    // guest takes no request of any channel and drops the HUD hide without it.
    void Shutdown() {
        if (pending_) store32_(OffsetCancelSeq, next_seq_);
        pending_ = false;
        store32_(OffsetModuleMagic, 0);
    }

    // The latest finished request, handed out once.
    bool TakeOutcome(Outcome& out) {
        if (!outcome_.done) return false;
        out = outcome_;
        outcome_ = {};
        return true;
    }

    std::uint32_t acks() const { return acks_; }

private:
    void PollPending(std::int64_t now_ms) {
        if (!pending_) return;
        std::uint32_t ack = 0;
        if (load32_(OffsetAckSeq, &ack) && ack == next_seq_) {
            std::uint32_t code = 0, value = 0;
            load32_(OffsetResultCode, &code);
            load32_(OffsetResultValue, &value);
            Finish(true, static_cast<Result>(code), value);
            return;
        }
        if (cancel_at_ < 0) {
            if (now_ms - staged_at_ > AckWindowMs()) {
                // The menu stopped waiting (the player used the native menu, the battle ended) or
                // the game paused: never let this request run on a later menu.
                store32_(OffsetCancelSeq, next_seq_);
                cancel_at_ = now_ms;
            }
            return;
        }
        if (now_ms - cancel_at_ > AckWindowMs()) {
            // Still unanswered: the guest will refuse it as Cancelled whenever it next runs.
            Finish(false, Result::Cancelled, 0);
        }
    }

    void Finish(bool answered, Result code, std::uint32_t value) {
        pending_ = false;
        if (answered) ++acks_;
        // A committed command closes the native menu: until the guest beats again (next menu),
        // the menu is not waiting -- otherwise a second tap inside the beat window would be
        // staged for a menu that is already gone (seen live 2026-10-26: RUN then a move tap).
        if (answered && code == Result::Ok) beat_at_ = -1;
        outcome_ = {true, answered, pending_op_, code, value};
        pending_op_ = Op::None;
    }

    LoadU32 load32_;
    StoreU32 store32_;
    Status status_{};
    Outcome outcome_{};
    std::uint32_t epoch_ = 0, beat_ = 0, next_seq_ = 0, acks_ = 0, alive_ = 0;
    bool beat_seen_ = false, pending_ = false, alive_seen_ = false;
    std::int64_t alive_at_ = -1, alive_frame_ms_ = 0;
    Op pending_op_ = Op::None;
    std::int64_t beat_at_ = -1, frame_ms_ = 0, staged_at_ = 0, cancel_at_ = -1;
};

// Module side of the HUD hide. Tick() runs on every host tick, the second screen shown or hidden
// (the module has on_action, so the host ticks it while hidden): `want` is "the setting keeps the
// battle HP on the bottom only" AND "the companion is on screen"; it is written every tick with a
// heartbeat, so the guest drops the hide by itself if the module ever stops.
//
// The setting is read with get_i64("@flag:battle.hp_top") (runtime 22 hosts from Eden Duo 1.3.2 on;
// an older host answers the fallback, and the plates remain visible). HudWant() is
// the whole rule.
struct HudStatus {
    bool layout_live = false;    // the battle HUD layout drew within the last second
    std::uint32_t hidden = 0;    // plates the helper holds hidden (bits 0-3)
    std::uint32_t found = 0;     // plates the helper found on its last frame (bits 0-3)
    bool requested = false;      // what this module asks for right now
};

// flag: 1 = "Battle HP on top" on, 0 explicitly off; an unknown value keeps the
// native plates visible. shown: __aux_present (1 when unknown).
inline bool HudWant(std::int64_t flag, bool shown) {
    return shown && flag == 0;
}
inline bool HudWantBattle(std::int64_t flag, bool shown, bool kind_current,
                          bool offline_single, bool raid_roster_ready) {
    return HudWant(flag, shown) && kind_current && (offline_single || raid_roster_ready);
}

template <class LoadU32, class StoreU32>
class HudHide {
public:
    HudHide(LoadU32 load32, StoreU32 store32) : load32_(std::move(load32)), store32_(std::move(store32)) {}

    bool Initialize() {
        requested_ = false;
        requested_mode_ = 0;
        load32_(OffsetHudFrame, &frame_);
        return store32_(OffsetHudReq, 0);
    }

    // hud_req is written on a change only (the module is its one writer); the heartbeat every tick.
    void Tick(bool want, std::int64_t now_ms, bool raid = false) {
        const std::uint32_t mode = want ? (raid ? 2u : 1u) : 0u;
        if (mode != requested_mode_) store32_(OffsetHudReq, mode);
        store32_(OffsetHudReqBeat, ++beat_);
        requested_ = want;
        requested_mode_ = mode;
        std::uint32_t frame = 0, state = 0;
        if (load32_(OffsetHudFrame, &frame) && frame != frame_) {
            frame_ = frame;
            frame_at_ = now_ms;
        }
        load32_(OffsetHudState, &state);
        status_.requested = want;
        status_.layout_live = frame_at_ >= 0 && now_ms - frame_at_ <= 1000;
        status_.hidden = status_.layout_live ? (state & 0xF) : 0;
        status_.found = status_.layout_live ? ((state >> 8) & 0xF) : 0;
    }

    // Module teardown: ask for the plates back (the heartbeat stops too, so the guest would
    // restore them anyway after HudStaleFrames).
    void Release() {
        requested_ = false;
        requested_mode_ = 0;
        store32_(OffsetHudReq, 0);
    }

    const HudStatus& status() const { return status_; }
    std::uint32_t beat() const { return beat_; }

private:
    LoadU32 load32_;
    StoreU32 store32_;
    HudStatus status_{};
    std::uint32_t beat_ = 0, frame_ = 0;
    std::int64_t frame_at_ = -1;
    bool requested_ = false;
    std::uint32_t requested_mode_ = 0;
};

// Battle-menu hide (2026-11-01, owner request; sw_guest_helper.S menu_entry). The module asks,
// with a heartbeat, and hands the guest every menu's cursor Layout resolved by ownership
// (sw_battle_cursor.h SampleLayouts); the guest clears/restores the panes itself every frame.
inline constexpr std::uint32_t OffsetMenuReq = 0x840;        // module: MenuReq bits
inline constexpr std::uint32_t OffsetMenuReqBeat = 0x844;    // module heartbeat
inline constexpr std::uint32_t OffsetMenuSeenBeat = 0x848;   // guest
inline constexpr std::uint32_t OffsetMenuStale = 0x84C;      // guest: hook calls since the beat moved
inline constexpr std::uint32_t OffsetMenuCursors = 0x868;    // module: 4 x u64 cursor Layouts
inline constexpr std::uint32_t OffsetMenuRecords = 0x888;    // guest: 10 x MenuRecord
inline constexpr int MenuSlots = 10;
inline constexpr std::uint32_t MenuRecordBytes = 24;         // {u64 pane, u64 layout, u32 serial, u32 hidden}
inline constexpr std::uint32_t MenuReqMenus = 1, MenuReqTarget = 2;
inline constexpr std::uint32_t MenuStaleCalls = 1200;

// "Battle menu on top" (@flag:battle.menu_top): default OFF = hidden (owner request); an unknown
// value (older host) keeps the native menus. Gated like the HP plates: the battle/raid page is on
// the bottom (shown), the battle kind is current, and the bottom is the full choice surface for
// this battle (an offline battle the companion drives: singles, raids, or the native-cursor drive
// of doubles / institution battles). The raid catch prompt (commandSelect_00 "ball") is never
// hidden. Target select is hidden only when the bottom has its synced target page.
inline std::uint32_t MenuWant(std::int64_t flag, bool shown, bool kind_current, bool drivable,
                              bool catch_prompt, bool target_page) {
    if (!shown || flag != 0 || !kind_current || !drivable || catch_prompt) return 0;
    return MenuReqMenus | (target_page ? MenuReqTarget : 0);
}

// MenuWant with a hold (owner Thor 2026-11-01, doubles: "some time top screen shown, sometime
// hidden"). What the player decides -- the setting, the companion shown on its battle page --
// and a catch prompt that is known to show act at once. The rest reads guest memory every tick
// (battle kind/serial, helper magic, the battle phase): a request standing for this battle stays
// through up to kHoldTicks consecutive ticks that drop it, so one failed load does not flash the
// native menus. The target bit is the caller's own (already held) reading, every tick.
class MenuWantFilter {
public:
    static constexpr int kHoldTicks = 30; // 0.5 s at 60 Hz
    std::uint32_t Step(std::int64_t flag, bool shown, bool kind_current, bool drivable, bool catch_prompt,
                       bool target_page) {
        if (!shown || flag != 0 || catch_prompt) return Reset();
        const std::uint32_t want = MenuWant(flag, shown, kind_current, drivable, false, target_page);
        if (want) {
            held_ = true;
            misses_ = 0;
            return want;
        }
        if (held_ && ++misses_ <= kHoldTicks) return MenuReqMenus | (target_page ? MenuReqTarget : 0);
        return Reset();
    }
    std::uint32_t Reset() {
        held_ = false;
        misses_ = 0;
        return 0;
    }

private:
    bool held_ = false;
    int misses_ = 0;
};

struct MenuStatus {
    std::uint32_t requested = 0;
    std::uint32_t hidden = 0; // bit = record slot the guest holds hidden
};

template <class LoadU32, class StoreU32>
class MenuHide {
public:
    MenuHide(LoadU32 load32, StoreU32 store32) : load32_(std::move(load32)), store32_(std::move(store32)) {}
    bool Initialize() {
        requested_ = 0;
        for (int i = 0; i < 8; ++i) store32_(OffsetMenuCursors + 4 * i, 0);
        return store32_(OffsetMenuReq, 0);
    }
    // cursors: 0 command, 1 move list, 2 ball picker, 3 target (0 = unknown -> that arrow stays).
    void Tick(std::uint32_t want, const std::uint64_t (&cursors)[4]) {
        for (int i = 0; i < 4; ++i) {
            if (cursors[i] == cursors_[i]) continue;
            store32_(OffsetMenuCursors + 8 * i, static_cast<std::uint32_t>(cursors[i]));
            store32_(OffsetMenuCursors + 8 * i + 4, static_cast<std::uint32_t>(cursors[i] >> 32));
            cursors_[i] = cursors[i];
        }
        if (want != requested_) store32_(OffsetMenuReq, want);
        store32_(OffsetMenuReqBeat, ++beat_);
        requested_ = want;
        status_.requested = want;
        status_.hidden = 0;
        for (int i = 0; i < MenuSlots; ++i) {
            std::uint32_t hidden = 0;
            if (load32_(OffsetMenuRecords + MenuRecordBytes * i + 20, &hidden) && hidden) status_.hidden |= 1u << i;
        }
    }
    void Release() {
        requested_ = 0;
        store32_(OffsetMenuReq, 0);
    }
    const MenuStatus& status() const { return status_; }

private:
    LoadU32 load32_;
    StoreU32 store32_;
    MenuStatus status_{};
    std::uint32_t beat_ = 0, requested_ = 0;
    std::uint64_t cursors_[4]{};
};

inline int HudCount(std::uint32_t bits) {
    int n = 0;
    for (int i = 0; i < HudPlates; ++i) n += (bits >> i) & 1;
    return n;
}

// Module side of the field channel. Same protocol as Bridge (seq/epoch/cancel/adaptive windows),
// its own offsets. `live` = the field beat advanced within FieldWindowMs. A running field process
// alone does not grant control; player_free and the module's phase gate are also required.
struct FieldStatus {
    bool helper_present = false;
    bool live = false;
    bool busy = false;
    bool player_free = false; // field_free on the latest field frame (the guest re-checks at execution)
    // Field frames seen advancing this session (the field hook's beat changes): proof the guest is
    // running the field. The battle phase uses it to tell "battle over" (field frames run, the
    // battle client is silent) from an emulation stall (nothing advances).
    std::uint64_t frames = 0;
};

struct FieldOutcome {
    bool done = false;
    bool answered = false;
    FieldOp op = FieldOp::None;
    Result code = Result::NotConfigured;
    std::uint32_t value = 0;
    std::uint64_t fly_mask = 0;  // FlyQuery: available destinations
    std::uint32_t fly_state = 0; // FlyQuery: 0 flying allowed here
    bool bike_usable = false;   // FlyQuery: the same native predicate Plus checks
    std::uint64_t map_zone = 0, map_parent = 0; // native display-parent lookup, paired zone guards stale data
};

template <class LoadU32, class StoreU32>
class FieldBridge {
public:
    FieldBridge(LoadU32 load32, StoreU32 store32) : load32_(std::move(load32)), store32_(std::move(store32)) {}

    bool Initialize() {
        std::uint32_t ack = 0, req = 0, active = 0;
        if (!load32_(OffsetFieldAckSeq, &ack) || !load32_(OffsetFieldReqSeq, &req) ||
            !load32_(OffsetFieldProgressionSeq, &active)) return false;
        next_seq_ = active ? active : (req ? req : ack);
        // Preserve a committed task's request/parameters and existing cancellation notification.
        // A new request must never reuse its sequence and mistake its eventual ACK for a new one.
        // Withdraw an unowned request that has not committed; never revive it by clearing CANCEL.
        return active != 0 || req == ack || store32_(OffsetFieldCancelSeq, req);
    }

    // The field beat comes every field frame; ~4 frames of silence (clamped 150-600 ms) = a menu,
    // a battle, a load, or a pause.
    std::int64_t FieldWindowMs() const { return std::clamp<std::int64_t>(frame_ms_ * 4, 150, 600); }
    std::int64_t AckWindowMs() const { return std::clamp<std::int64_t>(frame_ms_ * 3 + 50, 250, 900); }

    void Update(std::int64_t now_ms) {
        std::uint32_t magic = 0, epoch = 0, beat = 0;
        const bool magic_ok = load32_(OffsetGuestMagic, &magic) && magic == GuestMagic;
        const bool epoch_ok = load32_(EpochOffset, &epoch) && epoch != 0;
        if (epoch_ok && epoch != epoch_) {
            epoch_ = epoch;
            beat_at_ = -1; // a pause/invalidate: the field must be seen running again
        }
        if (load32_(OffsetFieldBeat, &beat) && beat != beat_) {
            if (beat_at_ >= 0) {
                const std::int64_t d = now_ms - beat_at_;
                if (d > 0 && d <= 1000) frame_ms_ = frame_ms_ ? (frame_ms_ * 3 + d) / 4 : d;
            }
            if (beat_seen_) {
                beat_at_ = now_ms;
                ++status_.frames;
            }
            beat_ = beat;
            beat_seen_ = true;
        }
        status_.helper_present = magic_ok && epoch_ok;
        PollPending(now_ms);
        status_.live = status_.helper_present && beat_at_ >= 0 && now_ms - beat_at_ <= FieldWindowMs();
        std::uint32_t free = 0;
        status_.player_free = status_.live && load32_(OffsetFieldFree, &free) && free == 1;
        std::uint32_t progression = 0;
        const bool progressing = load32_(OffsetFieldProgressionSeq, &progression) && progression != 0;
        status_.player_free = status_.player_free && !progressing;
        status_.busy = pending_ || progressing;
    }

    const FieldStatus& status() const { return status_; }
    FieldOp pending_op() const { return pending_ ? pending_op_ : FieldOp::None; }

    // Medicine: `ec` = the target slot's PK8 encryption constant (the guest compares it with the
    // live slot before CanUse, review M1).
    bool Stage(FieldOp op, std::uint32_t arg, std::uint32_t expect, std::int64_t now_ms,
               std::uint64_t zone = 0, std::uint64_t landing = 0, std::uint32_t ec = 0,
               std::uint32_t move = 0) {
        if (pending_ || status_.busy || !status_.helper_present || epoch_ == 0 || op == FieldOp::None) return false;
        bool ok = true;
        if (op == FieldOp::Fly) ok = Store64(OffsetFieldZone, zone) && Store64(OffsetFieldLanding, landing);
        if (op == FieldOp::Progression && (!IsFieldProgression(static_cast<int>(arg)) || expect > 5 || move != 0)) return false;
        if (op == FieldOp::PartySwap) {
            // Both slots and both members' encryption constants (the guest re-checks all of them
            // under the party lock and calls the party's own exchange, sw_guest_helper.S).
            if (arg > 5 || expect > 5 || arg == expect || ec == 0 || move == 0) return false;
            ok = store32_(OffsetFieldExpectEc, ec) && store32_(OffsetFieldMove, move);
        }
        if (op == FieldOp::Medicine || op == FieldOp::Progression) {
            if (move > 3) return false;
            ok = store32_(OffsetFieldExpectEc, ec) && store32_(OffsetFieldMove, move);
        }
        return ok && Send(op, arg, expect, now_ms);
    }

    // Module teardown (review L1): whatever is still staged must never run once the module is gone.
    void Shutdown() {
        if (pending_) store32_(OffsetFieldCancelSeq, next_seq_);
        pending_ = false;
    }

    // pairs: {zone, landing} per destination (at most FlyQueryMax).
    bool StageQuery(const std::uint64_t (*pairs)[2], std::uint32_t count, std::int64_t now_ms) {
        if (pending_ || status_.busy || !status_.helper_present || epoch_ == 0 || count == 0 || count > FlyQueryMax) return false;
        bool ok = store32_(OffsetFlyQueryCount, count);
        for (std::uint32_t i = 0; i < count && ok; ++i)
            ok = Store64(OffsetFlyQueryPairs + i * 16, pairs[i][0]) && Store64(OffsetFlyQueryPairs + i * 16 + 8, pairs[i][1]);
        return ok && Send(FieldOp::FlyQuery, 0, 0, now_ms);
    }

    bool TakeOutcome(FieldOutcome& out) {
        if (!outcome_.done) return false;
        out = outcome_;
        outcome_ = {};
        return true;
    }

private:
    bool Store64(std::uint32_t off, std::uint64_t v) {
        return store32_(off, static_cast<std::uint32_t>(v)) && store32_(off + 4, static_cast<std::uint32_t>(v >> 32));
    }

    bool Send(FieldOp op, std::uint32_t arg, std::uint32_t expect, std::int64_t now_ms) {
        std::uint32_t seq = next_seq_ + 1;
        if (seq == 0) seq = 1;
        bool ok = store32_(OffsetFieldReqOp, static_cast<std::uint32_t>(op));
        ok = store32_(OffsetFieldReqArg, arg) && ok;
        ok = store32_(OffsetFieldReqExpect, expect) && ok;
        ok = store32_(OffsetFieldReqEpoch, epoch_) && ok;
        ok = store32_(OffsetFieldReqSeq, seq) && ok; // release, last
        if (!ok) return false;
        next_seq_ = seq;
        pending_ = true;
        pending_op_ = op;
        pending_epoch_ = epoch_;
        staged_at_ = now_ms;
        cancel_at_ = -1;
        status_.busy = true;
        return true;
    }

    void PollPending(std::int64_t now_ms) {
        if (!pending_) return;
        std::uint32_t ack = 0;
        if (load32_(OffsetFieldAckSeq, &ack) && ack == next_seq_) {
            std::uint32_t code = 0, value = 0, lo = 0, hi = 0, state = 0;
            load32_(OffsetFieldResultCode, &code);
            load32_(OffsetFieldResultValue, &value);
            FieldOutcome o{true, true, pending_op_, static_cast<Result>(code), value};
            if (pending_op_ == FieldOp::FlyQuery && code == 0) {
                load32_(OffsetFlyQueryMask, &lo);
                load32_(OffsetFlyQueryMask + 4, &hi);
                load32_(OffsetFlyQueryState, &state);
                o.fly_mask = (static_cast<std::uint64_t>(hi) << 32) | lo;
                o.fly_state = state;
                std::uint32_t bike = 0;
                o.bike_usable = load32_(OffsetBikeUsable, &bike) && bike == 1;
                if (load32_(OffsetMapQueryZone, &lo) && load32_(OffsetMapQueryZone + 4, &hi))
                    o.map_zone = (static_cast<std::uint64_t>(hi) << 32) | lo;
                if (load32_(OffsetMapQueryParent, &lo) && load32_(OffsetMapQueryParent + 4, &hi))
                    o.map_parent = (static_cast<std::uint64_t>(hi) << 32) | lo;
            }
            Finish(o);
            return;
        }
        std::uint32_t active_progression = 0;
        if (pending_op_ == FieldOp::Progression &&
            load32_(OffsetFieldProgressionSeq, &active_progression) && active_progression == next_seq_) {
            // Guest owns stable parameters until the complete committed EXP/evolution/fade flow
            // finishes. Epoch changes notify it; they cannot split an already consumed item effect.
            if (pending_epoch_ != epoch_ && cancel_at_ < 0) {
                store32_(OffsetFieldCancelSeq, next_seq_);
                cancel_at_ = now_ms;
            }
            return;
        }
        if (cancel_at_ < 0) {
            if (now_ms - staged_at_ > AckWindowMs() && !status_.live) {
                // The field stopped (a menu, a battle, a load, a pause): never run it later.
                store32_(OffsetFieldCancelSeq, next_seq_);
                cancel_at_ = now_ms;
            } else if (now_ms - staged_at_ > AckWindowMs() * 4) {
                store32_(OffsetFieldCancelSeq, next_seq_);
                cancel_at_ = now_ms;
            }
            return;
        }
        if (now_ms - cancel_at_ > AckWindowMs()) Finish({true, false, pending_op_, Result::Cancelled, 0});
    }

    void Finish(const FieldOutcome& o) {
        pending_ = false;
        outcome_ = o;
        pending_op_ = FieldOp::None;
    }

    LoadU32 load32_;
    StoreU32 store32_;
    FieldStatus status_{};
    FieldOutcome outcome_{};
    std::uint32_t epoch_ = 0, beat_ = 0, next_seq_ = 0;
    bool beat_seen_ = false, pending_ = false;
    FieldOp pending_op_ = FieldOp::None;
    std::uint32_t pending_epoch_ = 0;
    std::int64_t beat_at_ = -1, frame_ms_ = 0, staged_at_ = 0, cancel_at_ = -1;
};

// Native own-card renderer: fresh automatic capture and host/native interruption verified.
inline constexpr bool FieldPortraitAutoVerified = true;
inline constexpr std::uint32_t OffsetPortraitReqSeq = 0x700;
inline constexpr std::uint32_t OffsetPortraitReqEpoch = 0x704;
inline constexpr std::uint32_t OffsetPortraitCancelSeq = 0x708;
inline constexpr std::uint32_t OffsetPortraitCapturedSeq = 0x70C;
inline constexpr std::uint32_t OffsetPortraitSeq = 0x710;
inline constexpr std::uint32_t OffsetPortraitPhase = 0x714;
inline constexpr std::uint32_t OffsetPortraitResult = 0x718;
inline constexpr std::uint32_t OffsetPortraitBeat = 0x71C;
inline constexpr std::uint32_t OffsetPortraitTarget = 0x720;
inline constexpr std::uint32_t OffsetPortraitOwnCard = 0x728;
inline constexpr std::uint32_t OffsetPortraitGlobal = 0x730;
inline constexpr std::uint32_t OffsetPortraitFieldApp = 0x738;
inline constexpr std::uint32_t OffsetPortraitWorker = 0x740;
inline constexpr std::uint32_t OffsetPortraitHeap = 0x748;
inline constexpr std::uint32_t OffsetPortraitRenderTicks = 0x750;
inline constexpr std::uint32_t OffsetPortraitCleanupTicks = 0x754;
inline constexpr std::uint32_t OffsetPortraitAppManager = 0x758;
inline constexpr std::uint32_t OffsetPortraitLifetimeFault = 0x760;
inline constexpr std::uint32_t OffsetPortraitCompleteSeq = 0x780;
enum class PortraitPhase : std::uint32_t { Idle, Rendering, Ready, Cancelling, FaultRetained };

namespace layout_check {
#pragma pack(push, 1)
struct Mailbox {
    std::uint32_t module_magic, guest_magic, wait_beat, pos_info;
    std::uint64_t client, mon;
    std::uint32_t req_seq, req_op, req_arg, req_expect, req_epoch, cancel_seq;
    std::uint32_t battle_kind, reserved0;
    std::uint32_t ack_seq, result_code, result_value, ack_beat;
    std::uint32_t alive_beat, reserved2;
    std::uint64_t alive_client;
    std::uint32_t hud_req, hud_req_beat, hud_frame, hud_state, hud_seen_beat, hud_stale;
    std::uint64_t hud_layout;
    std::uint64_t hud_pane[HudPlates];
    std::uint32_t hud_seen_battle_serial, battle_serial;
    std::uint64_t hud_seen_client;
    std::uint32_t dmax_opportunity, dmax_preview_beat;
    std::uint32_t dmax_ids[4], dmax_powers[4];
    std::int32_t dmax_gmax[4];
    std::uint64_t dmax_mon, dmax_client;
    std::uint32_t raid_target_mask, battle_kind_serial;
    std::uint32_t field_beat, field_free, reserved3[2];
    std::uint32_t freq_seq, freq_op, freq_arg, freq_expect, freq_epoch, fcancel_seq;
    std::uint64_t fzone, flanding;
    std::uint32_t fexpect_ec, fmove;
    std::uint32_t fack_seq, fresult_code, fresult_value, fack_beat;
    std::uint32_t fq_count, reserved5;
    std::uint64_t fq_mask;
    std::uint32_t fq_state, reserved6a, bike_usable;
    std::uint8_t reserved6b[OffsetFieldProgressionSeq - 0x16C];
    std::uint32_t field_progression_seq;
    std::uint8_t reserved6c[OffsetMapQueryZone - (OffsetFieldProgressionSeq + 4)];
    std::uint64_t map_query_zone;
    std::uint64_t map_query_parent;
    std::uint32_t dmax_base_ids[4];
    std::uint32_t hud_seen_mode;
    std::uint8_t reserved_preview[0x200 - 0x19C];
    std::uint64_t fq_pairs[FlyQueryMax * 2];
    std::uint8_t reserved7[OffsetCursorReqSeq - 0x200 - FlyQueryMax * 16];
    std::uint32_t cursor_req_seq, cursor_req_index, cursor_req_epoch, cursor_req_kind;
    std::uint64_t cursor_req_list, cursor_req_view, cursor_req_client;
    std::uint32_t cursor_ack_seq, cursor_result;
    std::uint8_t reserved9[OffsetMenuReq - (OffsetCursorResult + 4)];
    std::uint32_t menu_req, menu_req_beat, menu_seen_beat, menu_stale;
    std::uint8_t reserved10[OffsetMenuCursors - (OffsetMenuStale + 4)];
    std::uint64_t menu_cursors[4];
    std::uint8_t menu_records[MenuSlots * MenuRecordBytes];
    std::uint8_t reserved11[MailboxSize - (OffsetMenuRecords + MenuSlots * MenuRecordBytes) - 4];
    std::uint32_t epoch;
};
#pragma pack(pop)
static_assert(offsetof(Mailbox, guest_magic) == OffsetGuestMagic);
static_assert(offsetof(Mailbox, wait_beat) == OffsetWaitBeat);
static_assert(offsetof(Mailbox, pos_info) == OffsetPosInfo);
static_assert(offsetof(Mailbox, client) == OffsetClient);
static_assert(offsetof(Mailbox, mon) == OffsetMon);
static_assert(offsetof(Mailbox, req_seq) == OffsetReqSeq);
static_assert(offsetof(Mailbox, req_op) == OffsetReqOp);
static_assert(offsetof(Mailbox, req_arg) == OffsetReqArg);
static_assert(offsetof(Mailbox, req_expect) == OffsetReqExpect);
static_assert(offsetof(Mailbox, req_epoch) == OffsetReqEpoch);
static_assert(offsetof(Mailbox, cancel_seq) == OffsetCancelSeq);
static_assert(offsetof(Mailbox, battle_kind) == OffsetBattleKind);
static_assert(offsetof(Mailbox, dmax_opportunity) == OffsetDmaxOpportunity);
static_assert(offsetof(Mailbox, dmax_preview_beat) == OffsetDmaxPreviewBeat);
static_assert(offsetof(Mailbox, dmax_ids) == OffsetDmaxMoveIds);
static_assert(offsetof(Mailbox, dmax_powers) == OffsetDmaxPowers);
static_assert(offsetof(Mailbox, dmax_gmax) == OffsetDmaxGmaxIndices);
static_assert(offsetof(Mailbox, dmax_mon) == OffsetDmaxMon);
static_assert(offsetof(Mailbox, dmax_client) == OffsetDmaxClient);
static_assert(offsetof(Mailbox, raid_target_mask) == OffsetRaidTargetMask);
static_assert(offsetof(Mailbox, battle_kind_serial) == OffsetBattleKindSerial);
static_assert(offsetof(Mailbox, dmax_base_ids) == OffsetDmaxBaseIds);
static_assert(offsetof(Mailbox, hud_seen_mode) == OffsetHudSeenMode);
static_assert(offsetof(Mailbox, ack_seq) == OffsetAckSeq);
static_assert(offsetof(Mailbox, result_code) == OffsetResultCode);
static_assert(offsetof(Mailbox, result_value) == OffsetResultValue);
static_assert(offsetof(Mailbox, ack_beat) == OffsetAckBeat);
static_assert(offsetof(Mailbox, alive_beat) == OffsetAliveBeat);
static_assert(offsetof(Mailbox, alive_client) == OffsetAliveClient);
static_assert(offsetof(Mailbox, hud_req) == OffsetHudReq);
static_assert(offsetof(Mailbox, hud_req_beat) == OffsetHudReqBeat);
static_assert(offsetof(Mailbox, hud_frame) == OffsetHudFrame);
static_assert(offsetof(Mailbox, hud_state) == OffsetHudState);
static_assert(offsetof(Mailbox, hud_seen_beat) == OffsetHudSeenBeat);
static_assert(offsetof(Mailbox, hud_stale) == OffsetHudStale);
static_assert(offsetof(Mailbox, hud_layout) == OffsetHudLayout);
static_assert(offsetof(Mailbox, hud_pane) == OffsetHudPane);
static_assert(offsetof(Mailbox, hud_seen_battle_serial) == OffsetHudSeenBattleSerial);
static_assert(offsetof(Mailbox, battle_serial) == OffsetBattleSerial);
static_assert(offsetof(Mailbox, hud_seen_client) == OffsetHudSeenClient);
static_assert(offsetof(Mailbox, field_beat) == OffsetFieldBeat);
static_assert(offsetof(Mailbox, field_free) == OffsetFieldFree);
static_assert(offsetof(Mailbox, freq_seq) == OffsetFieldReqSeq);
static_assert(offsetof(Mailbox, freq_op) == OffsetFieldReqOp);
static_assert(offsetof(Mailbox, freq_arg) == OffsetFieldReqArg);
static_assert(offsetof(Mailbox, freq_expect) == OffsetFieldReqExpect);
static_assert(offsetof(Mailbox, freq_epoch) == OffsetFieldReqEpoch);
static_assert(offsetof(Mailbox, fcancel_seq) == OffsetFieldCancelSeq);
static_assert(offsetof(Mailbox, fzone) == OffsetFieldZone);
static_assert(offsetof(Mailbox, flanding) == OffsetFieldLanding);
static_assert(offsetof(Mailbox, fexpect_ec) == OffsetFieldExpectEc);
static_assert(offsetof(Mailbox, fmove) == OffsetFieldMove);
static_assert(offsetof(Mailbox, fack_seq) == OffsetFieldAckSeq);
static_assert(offsetof(Mailbox, fresult_code) == OffsetFieldResultCode);
static_assert(offsetof(Mailbox, fresult_value) == OffsetFieldResultValue);
static_assert(offsetof(Mailbox, fack_beat) == OffsetFieldAckBeat);
static_assert(offsetof(Mailbox, fq_count) == OffsetFlyQueryCount);
static_assert(offsetof(Mailbox, fq_mask) == OffsetFlyQueryMask);
static_assert(offsetof(Mailbox, fq_state) == OffsetFlyQueryState);
static_assert(offsetof(Mailbox, bike_usable) == OffsetBikeUsable);
static_assert(offsetof(Mailbox, fq_pairs) == OffsetFlyQueryPairs);
static_assert(offsetof(Mailbox, field_progression_seq) == OffsetFieldProgressionSeq);
static_assert(offsetof(Mailbox, map_query_zone) == OffsetMapQueryZone);
static_assert(offsetof(Mailbox, map_query_parent) == OffsetMapQueryParent);
static_assert(offsetof(Mailbox, cursor_req_seq) == OffsetCursorReqSeq);
static_assert(offsetof(Mailbox, cursor_req_index) == OffsetCursorReqIndex);
static_assert(offsetof(Mailbox, cursor_req_epoch) == OffsetCursorReqEpoch);
static_assert(offsetof(Mailbox, cursor_req_list) == OffsetCursorReqList);
static_assert(offsetof(Mailbox, cursor_req_view) == OffsetCursorReqView);
static_assert(offsetof(Mailbox, cursor_req_client) == OffsetCursorReqClient);
static_assert(offsetof(Mailbox, cursor_ack_seq) == OffsetCursorAckSeq);
static_assert(offsetof(Mailbox, cursor_result) == OffsetCursorResult);
static_assert(offsetof(Mailbox, cursor_req_kind) == OffsetCursorReqKind);
static_assert(offsetof(Mailbox, menu_req) == OffsetMenuReq);
static_assert(offsetof(Mailbox, menu_req_beat) == OffsetMenuReqBeat);
static_assert(offsetof(Mailbox, menu_seen_beat) == OffsetMenuSeenBeat);
static_assert(offsetof(Mailbox, menu_stale) == OffsetMenuStale);
static_assert(offsetof(Mailbox, menu_cursors) == OffsetMenuCursors);
static_assert(offsetof(Mailbox, menu_records) == OffsetMenuRecords);
static_assert(offsetof(Mailbox, epoch) == EpochOffset);
static_assert(sizeof(Mailbox) == MailboxSize);
} // namespace layout_check

} // namespace sw_helper
