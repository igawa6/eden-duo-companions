// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH 3.0.3 Bag page actions (bag lane): rearranging the pockets and changing the held tool by
// guarded direct writes, so the main screen never blips. Research: research/acnh/impl/write/PLAN.md
// (the recipes, byte-exact, from the game's code and native-move diffs) and
// research/acnh/impl/bag/REPORT.md (this lane).
//
// What the game does (main-relative code of the 3.0.3 main, build ff1d1c05670db602...):
//   SwapSlots 0x26fd1d8(Baggage = personal+0x37BF0, i, j): global index i < min(ItemPocket count,
//     20) -> ItemPocket[i], else ItemBag[i - that]; swaps the two 8-byte items and the two s8
//     favourite (= tool-ring position) bytes; then the relink 0x2716c30 points the ToolPack's
//     LinkHolder / IndexInHolder (personal+0x1B70 / +0x1B71) at wherever the held item now is.
//   SetTool 0x27166e0(ToolPack = personal+0x12C0, item, holder, index): an item whose hold class
//     (ItemOutfitInfo row of its ItemParam row, Storage column 0xC89FB7AF) has bit 9 as the lowest
//     set bit of bits 1..13 is copied to the ToolPack with its holder/index; anything else clears
//     it.
//   EquipTool 0x2371f38 then sets the hand model: comp = [[actor+0x298]+0x1480], SetItem 0x16327f0
//     stores the item at comp+0xD4. While the per-player held-item override ([main+0x53f5738]+0x284
//     +no*0xC, active byte +9 with a holdable item, 0x2447c90; set by event code, e.g. 0xb515a0) is
//     active, EquipTool updates that record instead and the Hold request is refused (0x2437490).
//   Hold in the X pocket menu (default builder 0x307e7e0, command 6): HoldRule below.
//
// The companion reproduces those stores (never calls game code) through the host's write_batch:
// every op carries the bytes the companion just read (expect), the batch is all-or-nothing and is
// applied with the guest threads suspended. Gates: the player is free (cWait/cMove/cTurn, camera
// cNormalField/cNormalRoom, phone idle, not loading: the pocket menu is closed), no other player in
// the session, no held-item override, every pinned code site matches, the personal save is
// resolved. A held-item change by direct write needs a tool already in the hand: the tool-out state
// is entered only by the game's own requests (0x2437348 from the pocket menu, 0x2435300 from the
// D-pad tool cycle), which are called from UI code or from the actor's per-frame pad poll 0x2223bf0
// (trigger bits actor+0xed0, rebuilt from the pad every frame at 0x2462074), never from a data
// field. With empty hands the companion writes the cycle's start slot (record+0x10) and presses
// DRight once (the game pulls that tool out itself); the pocket menu (X, cursor, A, Hold) remains
// the fallback.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

namespace acnh {
class Catalog;
namespace live {
struct LiveSnapshot;
class Guest;
} // namespace live

namespace bag {

// ---- personal.dat (GSavePersonal, 3.0.x schema) ------------------------------------------------
inline constexpr uint32_t ToolItem = 0x12C0; ///< Player.LookPack.Look.ToolPack.Tool.ItemName (8 B)
inline constexpr uint32_t ToolLink =
    0x1B70; ///< LinkHolder u8 (0 ItemPocket, 1 ItemBag), +1 IndexInHolder s8
inline constexpr uint32_t ItemBag = 0x37BF0;    ///< Item[20], u32 count +0xA0, s8 fav[20] +0xA4
inline constexpr uint32_t ItemPocket = 0x37CA8; ///< Item[20], u32 count +0xA0, s8 fav[20] +0xA4
inline constexpr uint32_t HolderCount = 0xA0, HolderFav = 0xA4;
inline constexpr uint32_t Span = 0x37D60 - ItemBag; ///< ItemBag .. end of ItemPocket's fav array
inline constexpr uint32_t CompItem = 0xD4; ///< tool component: the item the hand model shows

using Item8 = std::array<uint8_t, 8>;
inline constexpr Item8 EmptyItem{0xFE, 0xFF, 0, 0, 0, 0, 0, 0};
inline bool IsEmpty(const Item8& it) {
    return it[0] == 0xFE && it[1] == 0xFF;
}
inline uint16_t ItemId(const Item8& it) {
    return static_cast<uint16_t>(it[0] | it[1] << 8);
}

/// The bytes a recipe reads: the two holders (+counts, favourites) and the ToolPack link/item.
struct Pockets {
    std::array<uint8_t, Span> raw{}; ///< personal+0x37BF0 .. +0x37D60
    Item8 tool{};                    ///< personal+0x12C0
    std::array<uint8_t, 2> link{};   ///< personal+0x1B70 (holder, index; index 0xFF = none)

    /// From a personal.dat image (file or guest copy): false when it is too small.
    bool FromPersonal(const uint8_t* per, size_t size);
    static uint32_t HolderBase(int holder) {
        return holder == 0 ? ItemPocket : ItemBag;
    }
    uint32_t Count(int holder) const;
    Item8 ItemAt(int holder, int index) const;
    uint8_t FavAt(int holder, int index) const;
};

/// A companion slot: bag.{i} 0..19 = ItemPocket[i] (pocket menu rows 1-2), 20..39 = ItemBag[i-20]
/// (rows 3-4). Valid when the index is below min(that holder's count, 20), the bounds SwapSlots
/// applies (a slot outside them reads as empty there and is never a target here).
struct Slot {
    int holder = -1, index = -1;
    uint32_t ItemOff() const {
        return Pockets::HolderBase(holder) + static_cast<uint32_t>(index) * 8;
    }
    uint32_t FavOff() const {
        return Pockets::HolderBase(holder) + HolderFav + static_cast<uint32_t>(index);
    }
};
bool SlotOf(const Pockets& p, int display, Slot& out);
/// The slot the ToolPack links (LinkHolder/IndexInHolder), or -1.
int LinkedSlot(const Pockets& p);

/// One write: `off` is personal-relative (base Personal), relative to the tool component (Comp) or
/// to this player's tool record R = [[GOT 0x4ab4fb8]] + no*0x13F0 (Rec: runtime only, not in the
/// save).
struct Op {
    enum Base : uint8_t { Personal, Comp, Rec } base = Personal;
    uint32_t off = 0;
    uint8_t size = 0;
    std::array<uint8_t, 8> expect{}, value{};
};

/// Rearrange: swap companion slots a and b (items + favourite bytes; the ToolPack link follows the
/// held item). <= 5 ops. False (+why) when the move is not allowed.
bool BuildMove(const Pockets& p, int a, int b, std::vector<Op>& ops, std::string& why);
/// Change the held tool to slot s while a tool is out (comp_item = the hand's current item):
/// ToolPack item + link and the hand component, 3 ops. `holdable` = the SetTool test result.
bool BuildEquip(const Pockets& p, int s, const Item8& comp_item, bool holdable,
                std::vector<Op>& ops, std::string& why);
// ---- any-slot silent equip through the deferred request R+0x4E0 (r7-equip.md) -----------------
/// The request's consumer 0x244d080 scans the global slots from 0 and takes the FIRST item of the
/// requested outfit class; 0x2435300 builds {item, holder, index} from that scan alone and
/// cToolChange's enter (0x2293a60 -> 0x23758a0) calls EquipTool synchronously, so no other field
/// (cycle cursor, ToolPack link, a hint) can steer it. For a later slot s of the class the route
/// swaps s with the class's first slot f (the native SwapSlots bytes), lets the game take f, then
/// swaps back.
inline constexpr uint32_t RecCursorOff =
    0x10; ///< R+0x10: EquipTool stores 0x27169a0(ToolPack) there
inline constexpr uint32_t RecRequestOff =
    0x4E0; ///< R+0x4E0: the deferred request (class N / 1 put away)
/// 0x27169a0: the tool cursor value of a ToolPack link (ItemPocket index, ItemBag index + 0x14).
int32_t ToolCursorOf(const Slot& sl);
/// Before the L press: swap s<->f (items, favourites, the ToolPack link follows a held one) and
/// R+0x4E0 := cls (expect `req_now` = 0) in ONE batch. f == s or bad slots -> false (+why).
bool BuildReqSwap(const Pockets& p, int s, int f, uint32_t cls, std::vector<Op>& ops,
                  std::string& why);
/// After the game took f (ToolPack links f with s's item): swap back s<->f (the link follows to s)
/// and R+0x10 := ToolCursorOf(s) (expect `cursor_now` = ToolCursorOf(f), what EquipTool stored), so
/// personal.dat, main.dat and the record equal a native Hold of s byte for byte (live-proved).
bool BuildReqSwapBack(const Pockets& p, int s, int f, int32_t cursor_now, std::vector<Op>& ops,
                      std::string& why);
/// Put the held tool away: ToolPack item FE FF.., link 00 FF, hand component FE FF.., 3 ops.
bool BuildUnequip(const Pockets& p, const Item8& comp_item, std::vector<Op>& ops, std::string& why);
// ---- the tool ring (favourite star) -----------------------------------------------------------
/// The fav byte of a pocket slot is its tool-ring position: 0..7 clockwise from the top (the ring
/// HUD, LRingMenuTools, 8 L_Btn at 45 degrees), 0xFF = no star. The pocket menu's Favorite (command
/// 0x37) opens the ring's position picker; its commit 0x30e6d54..0x30e6dfc clears the old owner of
/// the chosen position (SetFav 0x2574f70(old, 0xFF) at 0x30e6da8) and sets the new slot
/// (SetFav(slot, pos) at 0x30e6df8); Clear Favorite (command 0x38, executor 0x3079d2c) is
/// SetFav(slot, 0xFF) alone. Nothing else in personal changes (native traces, explore_actions.md).
inline constexpr int RingPositions = 8;
inline constexpr uint8_t NoFav = 0xFF;
/// The ring position of a fav byte (0..7), else -1.
inline int RingPos(uint8_t fav) {
    return fav < RingPositions ? fav : -1;
}
/// The slot (display index) at each ring position, -1 = empty; `dups` = positions held by more than
/// one valid slot (never made by the game).
std::array<int, RingPositions> RingOwners(const Pockets& p, int* dups = nullptr);
/// Favourite: put slot s on ring position pos 0..7 (the old owner of pos loses its star first), or
/// pos -1 = Clear Favorite. <= 2 one-byte ops, byte-exact to the game's SetFav stores (a starred
/// item moved to another position = the game's Clear Favorite then Favorite, same bytes).
bool BuildFav(const Pockets& p, int s, int pos, std::vector<Op>& ops, std::string& why);
/// Rearrange favourites: swap occupied ring positions; centre items replace the old owner.
bool BuildRingMove(const Pockets& p, int s, int pos, std::vector<Op>& ops, std::string& why);
/// The menu's Favorite offer for an item, 0x19dbe40(item, 1, 0, 0, 1) (0x307f854..0x307f96c): the
/// SetTool hand-tool Storage rule and bit 4 of ItemOutfitCategory.Misc (column 0x42AD246A) of the
/// item's outfit class row clear (0x19dc088..0x19dc1a4; in 3.0.3 only row 22 FenceMaker has it).
/// The menu also needs EventFlag ItemRingEnable and scene attribute bit 0x19 clear (Writer).
bool FavItemRule(uint16_t storage, uint8_t category_misc);
inline constexpr uint32_t CategoryMiscKey = 0x42AD246Au;

// ---- the DIY app's Favorite (recipe star, r9 polish) --------------------------------------------
/// personal.dat GSaveCraftingRecipeCollect (0x6528C, 0x1400): RecipeCollectBit, +0x100 Made,
/// +0x200 New, +0x300 RecipeFavoriteBit, one bit per recipe UniqueID (< 0x800).
inline constexpr uint32_t RecipeCollectBits = 0x6528C, RecipeFavBits = 0x6558C;
/// The DIY app's Favorite toggle (X on a recipe, 0x2e9f050): its list entry's favourite byte
/// (+0x27) picks RecipeFavOn 0x252f2e8 (bit uid of RecipeFavoriteBit |= ) or RecipeFavOff 0x252f518
/// (&= ~); nothing else in the save changes (no count, no limit, the New bit untouched; LIVE diff
/// in r9-polish.md). Built from the two bytes just read (collect / fav byte of uid >> 3): one
/// 1-byte expect-guarded op; `want` already the state -> no op and true; an unknown recipe or uid
/// >= 0x800
/// -> false (+why).
bool BuildRecipeFav(uint8_t collect_byte, uint8_t fav_byte, unsigned uid, bool want,
                    std::vector<Op>& ops, std::string& why);

/// Apply ops to a personal.dat image (tests): true when every expect matched.
bool ApplyToImage(const std::vector<Op>& ops, std::vector<uint8_t>& personal, Item8* comp = nullptr,
                  std::vector<uint8_t>* rec = nullptr);

/// SetTool's hand-tool test on an ItemOutfitInfo Storage value (column 0xC89FB7AF, first u16):
/// the lowest set bit among bits 1..13 must be bit 9 (loop main 0x2716804..0x2716874).
bool StorageIsHandTool(uint16_t storage);
inline constexpr uint32_t OutfitStorageKey = 0xC89FB7AFu;
inline constexpr uint32_t OutfitRows = 0xA2; ///< 0x19dab50: outfit row <= 0xA1, else row 0

// ---- the pocket menu's "Hold" rule (code) ------------------------------------------------------
/// What the game's X pocket menu offers for an item (default command builder 0x307e7e0, command 6):
/// Hold iff kind(row) != 0x78 (fences: "Build Fence") and F2 0x19dd954(item, {1, 0}): the outfit
/// class is 0x43, or (plain scene) the Storage value of its ItemOutfitInfo row is a hand tool
/// (lowest set bit of bits 1..13 is bit 9), with scene attribute bit 0x17 also 0x19dc580's kind
/// test. Scenes with attribute bits 0/5/6/7 run other builders' rules (0x19dce10 / 0x19dc980):
/// Unknown.
enum class Hold : uint8_t { Unknown, No, Yes };
struct HoldInputs {
    bool row = false; ///< the effective id has an ItemParam row (else the menu has no commands)
    bool kind_known = false; ///< kind table [GOT 0x4ac00c8][row index] readable
    uint16_t kind = 0;
    bool class43 = false;    ///< outfit class 0x43 (0x19d78cc): Hold without the Storage test
    uint16_t storage = 0;    ///< ItemOutfitInfo Storage (column 0xC89FB7AF) of the hold-class row
    int16_t scene = -1;      ///< s16 [main+0x4b487d4]
    uint64_t scene_mask = 0; ///< attribute bits of that scene
};
/// The rule itself (pure, tested): Yes / No, Unknown in scenes whose builder path is not
/// reproduced.
Hold HoldRule(const HoldInputs& in);
/// 0x19dc580's kind part: kind > 0x20 or bit `kind` of 0x40ffffff (kind: no row 0xbc, else <= 0xbc
/// or 0).
bool KindAllowsHold(uint16_t kind);
/// The effective ItemParam id of a pocket item (0x267d6c4): wrapped items stand for their wrapping.
/// `wrap_color` = u32[4] at main+0x522a758, `tab1` = u32[17] at main+0x4077938, `tab4` = u32[4] at
/// main+0x4001d00 (read from the guest).
uint16_t EffectiveId(const Item8& it, const uint32_t* wrap_color, const uint32_t* tab1,
                     const uint32_t* tab4);

// ---- live writer -------------------------------------------------------------------------------
/// The Bag page's write path: gates, the action queue (one batch per action, applied by the next
/// sample after a fresh read), the empty-hands tool-cycle press and the pocket-menu drive.
/// Timing thread only.
class Writer {
public:
    explicit Writer(Catalog* catalog) : catalog{catalog} {}
    void SetWriteApi(const EdenDsmodHostWriteApi& api) {
        write = api;
        have_write = true;
    }
    /// After the publisher: re-reads, applies a queued action, steps the drive, publishes
    /// bag.can_write, bag.msg, bag.card, the per-slot hok / fok gates, equip.*, ring.* and (while a
    /// drive runs) the shared pdrv.* gates, and bag.sel.nohold (r9, owner: 1 = the page's selected
    /// slot `selected` holds an item the held-item circle cannot take: the circle dims).
    void Sample(const EdenDsmodHostApi& host, const live::Guest& g, const live::LiveSnapshot& s,
                uint64_t ui_root, bool page_open, bool phone_drive_busy, int selected = -1);
    /// bag_move (argument from*64+to), bag_drop_<to> (argument = the dragged payload: a slot, or
    /// 100 = the equipped circle -> unequip), bag_equip <slot>, bag_unequip, bag_writes 0/1,
    /// bag_star <slot> (opens the ring picker for that slot), bag_ring <pos 0..7, 8 = clear>,
    /// bag_ring_close, bag_card_tap (dismisses a refusal card).
    /// Returns false (refused haptic) when the request cannot run: decided at once from the last
    /// sample's read (gates, empty slot, the Hold rule), published in the same tick.
    bool Action(std::string_view name, int64_t arg, bool& handled);
    bool DriveBusy() const {
        return drive.state != Drive::Idle;
    }

private:
    enum class Kind { None, Move, Equip, Unequip, Fav, RingMove, RecipeFav };
    struct Request {
        Kind kind = Kind::None;
        int a = -1, b = -1;
        Item8 item{}; ///< Fav: the item the picker was opened for (the slot must still hold it)
    };
    struct Drive {
        // Cycle*: the game's own D-pad tool cycle (one press, no menu); Open..Hold: the pocket menu
        // Req*: the deferred tool request R+0x4E0 (class N = take the first pocket item of outfit
        // class N, 1 = put away) consumed at the next EnterWait, triggered by one L press
        enum State {
            Idle,
            ReqSet,
            ReqPress,
            ReqWait,
            CycleSet,
            CyclePress,
            CycleWait,
            Open,
            Cursor,
            Submenu,
            Hold,
            Wait
        } state = Idle;
        int target = -1;
        int steps = 0, settle = 0, waiting = 0, cycle_tries = 0;
        uint32_t req_cls = 0; ///< the class written to R+0x4E0 (1 = put away)
        int req_first =
            -1; ///< the class's first slot swapped with the target for the request (-1 none)
        bool req_owned = false; ///< this drive successfully placed the pending request
        bool cancelled = false; ///< only owned cleanup may run after a gate closes
        uint64_t personal = 0, actor = 0, record = 0; ///< identity at drive acceptance
        Item8 displaced{};        ///< original item in the temporary first-class slot
        bool req_swapped = false; ///< the swap is in the pockets now (swap back before finishing)
        bool req_withdrawn =
            false; ///< not taken, swapped back: fall back on the next (fresh) sample
        int req_tries = 0;
        uint64_t start_ms = 0;
        std::string seen;
        Item8 item{};
    };
    struct View; ///< one fresh read of everything the gates and recipes need
    struct Verdict {
        Hold hold = Hold::Unknown;
        bool class43 = false;
        std::string why;
    };
    bool ReadView(const live::Guest& g, const live::LiveSnapshot& s, uint64_t ui_root, View& v);
    struct ItemFacts {
        bool row = false, kind_known = false;
        uint16_t kind = 0, storage = 0;
        int cls = 0;      ///< outfit class (0x19d78cc), 0 when none
        uint8_t misc = 0; ///< ItemOutfitCategory.Misc of row cls (0x19d81b0 -> column 0x42AD246A)
    };
    /// The game's per-item tables for one pocket item (effective id, row, kind, outfit class,
    /// Storage).
    bool Facts(const live::Guest& g, const View& v, const Item8& it, ItemFacts& f,
               std::string& why);
    Verdict HoldOf(const live::Guest& g, const View& v, const Item8& it);
    /// The Hold rule on facts already read (v.scene_ok).
    static Verdict HoldFrom(const View& v, const ItemFacts& f);
    bool OverrideActive(const live::Guest& g, const View& v, int player_no, bool& ok);
    bool Apply(const View& v, const std::vector<Op>& ops);
    bool ApplyAbs(uint64_t at, const uint8_t* expect, const uint8_t* value, uint8_t size);
    void StepDrive(const live::Guest& g, const live::LiveSnapshot& s, const View& v, bool view_ok,
                   bool forward_allowed, std::string& press);
    void StartMenuDrive();
    void Finish(int result, std::string msg);
    void PublishNow(); ///< bag.msg / card / ring.open in the current tick (Action)
    /// The blocking card over the Pockets page (bag.card code, 0 = none) at `now`.
    int CardCode(uint64_t now) const;
    void PublishCard(const EdenDsmodHostApi& host, uint64_t now);
    bool CheckPins(const live::Guest& g);
    int Global(const Pockets& p, const Slot& sl) const; ///< the game's contiguous slot index
    /// The request route's class for slot s (its outfit class, 0 = no route) and `first`, the slot
    /// the game's scan takes for that class (0x244d080: global slots from 0); first != s -> the
    /// route swaps.
    uint32_t ReqClassFor(const Pockets& p, int s, int& first) const;
    /// After the request route failed: equip by the direct swap / cycle / pocket menu, unequip
    /// directly.
    void ReqFallback(const live::LiveSnapshot& s, const View& v);

    Catalog* catalog;
    EdenDsmodHostWriteApi write{};
    bool have_write = false;
    EdenDsmodHostApi host_copy{};
    bool have_host = false;
    bool enabled = true;
    uint64_t pins_base = 0;
    bool pins_ok = false;
    std::array<uint64_t, 16>
        addr{}; ///< absolute addresses from the pins (AddrId in acnh_bag_live.cpp)
    Request req;
    Drive drive;
    int result =
        0; ///< last request: 0 none, 1 done, 2 refused, 3 failed (expect mismatch / timeout)
    std::string msg; ///< bag.msg (cleared 5 s after the request finished)
    uint64_t msg_ms = 0;
    // cached published state (the last sample), used by Action for an immediate verdict
    bool equipment_allowed = false;
    bool can_diy = false;
    bool can_write = false, can_equip = false, can_drive = false, tool_out = false;
    std::string why;
    int linked = -1;
    std::array<Hold, 40> slot_hold{};
    std::array<bool, 40> slot_used{};
    std::array<bool, 40> slot_in{};
    uint64_t hold_key = 0; ///< pockets + scene hash of slot_hold
    // tool ring: per-slot Favorite rule (item part), the menu's global gates, the open picker
    std::array<bool, 40> slot_fav{};
    std::array<Item8, 40> slot_item{};
    std::array<int, 40> slot_cls{}; ///< outfit class (ItemOutfitCategory row) per slot, -1 unknown
    bool ring_enable =
        false;               ///< EventFlag ItemRingEnable (Player EventFlag, EventFlagsPlayerParam)
    bool ring_scene = false; ///< scene attribute bit 0x19 clear (the menu offers Favorite there)
    int ring_flag_uid = -2;  ///< UniqueID of ItemRingEnable (-2 = not looked up)
    bool ring_open = false;
    int ring_center = -1, ring_selected = -1;
    std::array<int, RingPositions> ring_owners{-1, -1, -1, -1, -1, -1, -1, -1};
    // blocking card: since when the writes are gated, the last refused attempt, dismissal
    std::string card_why;    ///< the gate the timer runs for
    uint64_t gate_ms = 0;    ///< when that gate started (0 = not gated)
    uint64_t attempt_ms = 0; ///< a user action refused at this time (shows the card at once)
    bool dismissed = false;  ///< the refusal card was tapped away
};

} // namespace bag
} // namespace acnh
