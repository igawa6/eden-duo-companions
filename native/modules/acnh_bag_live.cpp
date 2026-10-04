// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH Bag page actions: the live writer (acnh_bag.h). Every address comes from the snapshot's
// code-resolved roots (personal.dat, PlayerActor, UI root) and the pinned code below; every write
// is one expect-guarded write_batch built from a read made in the same sample.

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>

#include "acnh_bag.h"
#include "acnh_bcsv.h"
#include "acnh_catalog.h"
#include "acnh_lang.h"
#include "acnh_live.h"
#include "acnh_pins.h"

namespace acnh::bag {
namespace {

using BagPin = CodePin; // the generated .inc names the type BagPin
#include "acnh_bag_pins.inc"

const BagPin* const AllPins[] = {
    &PinBagSwap,     &PinBagSwapFav, &PinToolSet,      &PinToolSetIndex, &PinHoldBits,
    &PinItemRow,     &PinOutfitRow,  &PinOutfitClass,  &PinOutfitClass2, &PinEffItem,
    &PinWrapColor,   &PinToolComp,   &PinCompSetItem,  &PinEquipNet,     &PinNetRec,
    &PinReqOverride, &PinHoldCmd,    &PinHoldDispatch, &PinHoldF2,       &PinHoldF1,
    &PinKindOk,      &PinHandTool,   &PinSession,      &PinSessionMode,  &PinCycleRec,
    &PinCycleStart,  &PinCycleTake,  &PinCyclePoll,    &PinPadCycle,     &PinPocketSlot,
    &PinPocketVt,    &PinPocketSm,   &PinPocketCursor, &PinPocketCmd};

bool IsHeap(uint64_t p) {
    return p >= 0x1000 && p < (uint64_t{1} << 39) && (p & 7) == 0;
}

// Per-slot / per-position published names, built once (not ~100 strings every sample).
struct PubNames {
    std::array<std::string, 40> hok, fok; // bag.<i>.hok / .fok
    std::array<std::string, RingPositions> ring_slot, ring_eq,
        ring_icon; // ring.<p>.slot / .eq / .icon
};
const PubNames& Names() {
    static const PubNames n = [] {
        PubNames t;
        for (int i = 0; i < 40; ++i) {
            t.hok[i] = "bag." + std::to_string(i) + ".hok";
            t.fok[i] = "bag." + std::to_string(i) + ".fok";
        }
        for (int p = 0; p < RingPositions; ++p) {
            const std::string k = "ring." + std::to_string(p);
            t.ring_slot[p] = k + ".slot";
            t.ring_eq[p] = k + ".eq";
            t.ring_icon[p] = k + ".icon";
        }
        return t;
    }();
    return n;
}

// The shared button drive's gates (manifest enforce): press name -> pdrv.<press>
constexpr std::pair<std::string_view, const char*> PdrvButtons[] = {
    {"a", "pdrv.a"},   {"b", "pdrv.b"},       {"x", "pdrv.x"},       {"zl", "pdrv.zl"},
    {"up", "pdrv.up"}, {"down", "pdrv.down"}, {"left", "pdrv.left"}, {"right", "pdrv.right"},
    {"l", "pdrv.l"},   {"r", "pdrv.r"}};

// Addresses decoded from the pins (main-relative), index into Writer::addr (absolute).
enum AddrId : size_t {
    AKindGot,   ///< GOT -> kind table {u32 count, +8 u16[]} per ItemParam row (HoldCmd 0x307fb6c)
    ASceneId,   ///< s16 current scene (HoldF2 0x19ddb50)
    AMaskGot,   ///< GOT -> scene attribute masks u32[2] per scene id (HoldF2 0x19ddb7c)
    ASession,   ///< session pointer (Session 0x307c32c)
    ACycleGot,  ///< GOT -> pointer to the per-player tool records (CycleRec 0x2482f04)
    AWrapColor, ///< u32[4] wrapping colours (WrapColor 0x1a03624)
    AWrapTab1,  ///< u32[17] wrapping ids (EffItem 0x267d738)
    AWrapTab4,  ///< u32[4] (EffItem 0x267d77c)
    AOverride,  ///< pointer to the player records holding the override at +0x284 (NetRec 0x2447cb0)
    AItemRowGot, ///< GOT -> pointer to the ItemParam table (ItemRow)
    AOutfitGot,  ///< GOT -> hold-class table {u32 count, +8 u16[]} (OutfitRow)
    AOutfitCls,  ///< pointer to the ItemOutfitInfo table object (OutfitClass)
    AOutfitCls2, ///< outfit class table {u32 count, +8 u16[]} (OutfitClass2)
    ACount
};
static_assert(ACount <= 16);

// Display index -> pocket-menu grid (10 columns; 0..19 = ItemPocket rows 1-2, 20..39 = ItemBag rows
// 3-4, the menu's own focus index, PocketCursor pin).
constexpr int MenuCols = 10;
// Pocket menu (PocketSm / PocketCmd pins): state machine at +0x1a8, command window [+0x218].
constexpr uint32_t MenuSm = 0x1a8, MenuWindow = 0x218, MenuCursor = 0x1c515;
constexpr uint32_t WinCount = 0x26d8, WinEntries = 0x238, WinCmdId = 0x168;
constexpr uint32_t WinState = 0x1b0,
                   WinOpen = 3;   ///< window state: 3 while the commands are shown (live), 1 closed
constexpr uint32_t CmdHold = 0x6; ///< command id of "Hold" (builder 0x307fc70)
// ItemParam row table (ItemRow pin).
constexpr uint32_t RowBase = 0x1f8, RowCount = 0x1d8, RowArray = 0x1e0;
// Per-player tool record (CycleRec / CycleStart pins): stride, cursor, validity bytes.
constexpr uint32_t RecStride = 0x13f0, RecCursor = 0x10, RecValid = 0x13d4, RecValid2 = 0x13d6;
// The deferred tool request u32 R+0x4E0 (equip_trigger.md): read only by 0x244d010 (in 0x244cf54)
// from EnterWait 0x226a88c, i.e. on the next return to cWait (L: cEntryFixDirWalkAction ->
// cFixDirWalkWait
// -> ReturnToFree); 1 = put the tool away, N >= 2 = the first global pocket slot whose outfit class
// is N (0x244d080), then 0x2435300 (the game's own tool-change gates, cToolChange) and the store
// wzr at 0x226a9fc. Not cleared when 0x2435300 refuses: the module clears it (expect = its class).
constexpr uint32_t RecRequest = 0x4e0;
constexpr int ReqWaitMax = 150; ///< samples (~2.5 s) for the game to take the request after L
// Held-item override record (NetRec pin): [g]+0x284+no*0xc {item 8 B, u16 flags}, active byte +9.
constexpr uint32_t OverrideRec = 0x284, OverrideStride = 0xc;

/// The blocking card's icon for a gate / refusal (bag.card): what the Pockets page shows over
/// itself while its writes are gated (the game's own words and art in the manifest: the dialog
/// window CmnDialog#CmnDialogWinBase^s with DIALOG_WherearenMsg 4009 "You can't do that right
/// now.", an icon per reason).
enum Card : int {
    CardNone = 0,
    CardLoading = 1,  ///< the game is loading / no save or player yet (map loading icon)
    CardMenu = 2,     ///< a game menu is open (Pockets icon, crossed)
    CardPhone = 3,    ///< the NookPhone is open or driven (NookPhone icon, crossed)
    CardOnline = 4,   ///< other players in the session (Best Friends app icon, crossed)
    CardBusy = 5,     ///< the player is busy / an event holds the item (reaction "Hesitate")
    CardRefused = 6,  ///< the last request was refused (reaction "Aha": the game's "!")
    CardWorking = 7,  ///< a request drives the game's own menu (loading icon, no text)
    CardUnusable = 8, ///< writes unavailable here (runtime / version / switched off: "?")
    CardEvent = 9,    ///< an event holds the items (held-item override; text lane: its own line)
};
int CardOf(const std::string& why) {
    if (why == "loading" || why == "save" || why == "player")
        return CardLoading;
    if (why == "menu open")
        return CardMenu;
    if (why == "ring open") // the menu reason; bag.card.ring tells the page to show the ring's icon
        return CardMenu;
    if (why == "phone open" || why == "phone drive")
        return CardPhone;
    if (why == "online")
        return CardOnline;
    if (why == "override")
        return CardEvent;
    if (why == "busy")
        return CardBusy;
    return CardUnusable; // "no write_batch", "off", "code"
}
constexpr uint64_t CardAfterMs = 1000;   ///< a gate shows the card once it lasts this long
constexpr uint64_t CardAttemptMs = 2500; ///< a refused attempt shows it this long

uint64_t Mix(uint64_t h, uint64_t v) {
    return (h ^ v) * 0x100000001b3ULL;
}

} // namespace

struct Writer::View {
    uint64_t per = 0, actor = 0, comp = 0, menu = 0, window = 0;
    Pockets p;
    Item8 comp_item{};
    bool comp_ok = false;
    bool online = false, session_ok = false; ///< session gate (Session / SessionMode pins)
    bool override_on = false, override_ok = false;
    uint64_t rec = 0; ///< this player's tool record (0 = none)
    int32_t cursor_rec = -1;
    int16_t scene = -1;
    uint64_t scene_mask = 0;
    bool scene_ok = false;
    uint32_t wrap_color[4]{}, tab1[17]{}, tab4[4]{};
    bool wrap_ok = false;
    int cursor = -1; ///< pocket menu focus (0..39), -1 none
    std::string menu_state;
    int win_count = 0;
    uint32_t win_first = 0, win_state = 0;
    int ring_flag = -1; ///< EventFlag ItemRingEnable (-1 unknown)
    uint32_t req = 0;   ///< R+0x4E0 (valid when rec != 0)
};

bool Writer::CheckPins(const live::Guest& g) {
    if (pins_base == g.Base())
        return pins_ok;
    pins_base = g.Base();
    pins_ok = true;
    for (const BagPin* p : AllPins)
        pins_ok = PinMatches(g, *p) && pins_ok;
    const std::pair<const BagPin*, std::pair<size_t, size_t>> at[ACount] = {
        {&PinHoldCmd, {92, 93}},   {&PinHoldF2, {127, 128}},  {&PinHoldF2, {138, 140}},
        {&PinSession, {0, 1}},     {&PinCycleRec, {4, 6}},    {&PinWrapColor, {5, 6}},
        {&PinEffItem, {29, 30}},   {&PinEffItem, {46, 47}},   {&PinNetRec, {8, 9}},
        {&PinItemRow, {0, 2}},     {&PinOutfitRow, {24, 25}}, {&PinOutfitClass, {0, 1}},
        {&PinOutfitClass2, {0, 1}}};
    addr.fill(0);
    for (size_t i = 0; i < ACount; ++i) {
        addr[i] = PinData(g, *at[i].first, at[i].second.first, at[i].second.second);
        if (!addr[i])
            pins_ok = false;
    }
    return pins_ok;
}

int Writer::Global(const Pockets& p, const Slot& sl) const {
    // 0x2577ea0 / 0x2572f60: ItemPocket first (min(count, 20)), then ItemBag, contiguous
    return sl.holder == 0 ? sl.index
                          : static_cast<int>(std::min<uint32_t>(p.Count(0), 20)) + sl.index;
}

uint32_t Writer::ReqClassFor(const Pockets& p, int s, int& first) const {
    first = -1;
    if (s < 0 || s >= 40 || !slot_in[s] || !slot_used[s] || slot_cls[s] < 2)
        return 0; // class 1 is "put away": never written for an item; 0 / unknown: no route
    // the game's scan: global slots from 0 (ItemPocket first, then ItemBag, contiguous)
    int best = -1, best_g = 1 << 30;
    for (int i = 0; i < 40; ++i) {
        Slot sl;
        if (slot_cls[i] < 0 && slot_used[i])
            return 0; // an unreadable slot could be the first of the class: do not guess
        if (slot_cls[i] != slot_cls[s] || !SlotOf(p, i, sl))
            continue;
        const int gi = Global(p, sl);
        if (gi < best_g) {
            best_g = gi;
            best = i;
        }
    }
    if (best < 0)
        return 0;
    first = best;
    return static_cast<uint32_t>(slot_cls[s]);
}

bool Writer::ReadView(const live::Guest& g, const live::LiveSnapshot& s, uint64_t ui_root,
                      View& v) {
    v = {};
    v.per = s.personal_ok ? s.personal_at : 0;
    v.actor = s.actor_at;
    if (!v.per || !IsHeap(v.per))
        return false;
    // pockets + favourites + counts, torn-checked; ToolPack item and link
    std::array<uint8_t, Span> a{}, b{};
    if (!g.Read(v.per + ItemBag, a.data(), Span) || !g.Read(v.per + ToolItem, v.p.tool.data(), 8) ||
        !g.Read(v.per + ToolLink, v.p.link.data(), 2) || !g.Read(v.per + ItemBag, b.data(), Span) ||
        a != b)
        return false;
    v.p.raw = a;
    // EventFlag ItemRingEnable (SetFav 0x2574f70 needs it for a position; the menu offers Favorite
    // only with it): Player.EventFlag u16[UniqueID], UniqueID from EventFlagsPlayerParam (the
    // publisher's rule)
    if (ring_flag_uid == -2) {
        ring_flag_uid = -1;
        if (const Bcsv* t = catalog ? catalog->Table("EventFlagsPlayerParam") : nullptr) {
            const uint32_t kkey = Bcsv::Key("Key", "string64"), kuid = Bcsv::Key("UniqueID", "u16");
            for (size_t r = 0; r < t->Rows() && ring_flag_uid < 0; ++r)
                if (t->Str(r, kkey) == "ItemRingEnable")
                    ring_flag_uid = static_cast<int>(t->U(r, kuid));
        }
    }
    uint16_t flag = 0;
    if (ring_flag_uid >= 0 && ring_flag_uid < 2048 &&
        g.Get(v.per + live::P::EventFlag + static_cast<uint64_t>(ring_flag_uid) * 2, flag))
        v.ring_flag = flag;
    // hand: comp = [[actor+0x298]+0x1480] (ToolComp pin), item at +0xD4 (CompSetItem pin)
    uint64_t c1 = 0;
    v.comp_ok = v.actor && IsHeap(v.actor) && g.Get(v.actor + 0x298, c1) && IsHeap(c1) &&
                g.Get(c1 + 0x1480, v.comp) && IsHeap(v.comp) &&
                g.Read(v.comp + CompItem, v.comp_item.data(), 8);
    if (!v.comp_ok)
        v.comp = 0;
    // session (Session / SessionMode pins): players in the session u8 [[s]+0x476] > 1 (the pocket
    // menu's own refusal 0x307c33c), or a session mode u32 [s+0x80] != 0 (TodayVisitor 0x27d6bb0
    // treats 1 as online; 2 / 3 unknown -> fail closed). Offline (live): [s+0x80] 0, [[s]+0x476] 0.
    uint64_t sess = 0, sobj = 0;
    if (addr[ASession] && g.Get(addr[ASession], sess)) {
        if (!sess) {
            v.session_ok = true;
        } else if (IsHeap(sess) && g.Get(sess, sobj) && IsHeap(sobj)) {
            uint8_t players = 0;
            uint32_t mode = 0;
            if (g.Get(sobj + 0x476, players) && g.Get(sess + 0x80, mode)) {
                v.session_ok = true;
                v.online = players > 1 || mode != 0;
            }
        }
    }
    // per-player tool record: R = [[GOT]] + no*0x13f0 (0x2482d8c: offline index = player no when
    // R+0x13d4 is set; 0x2223bf0 also needs +0x13d6), cursor u32 at +0x10
    uint64_t rgot = 0, r0 = 0;
    if (s.player_no >= 0 && s.player_no < 8 && addr[ACycleGot] && g.Get(addr[ACycleGot], rgot) &&
        rgot && g.Get(rgot, r0) && IsHeap(r0)) {
        const uint64_t rec = r0 + uint64_t(s.player_no) * RecStride;
        uint8_t v1 = 0, v2 = 0;
        if (g.Get(rec + RecValid, v1) && g.Get(rec + RecValid2, v2) && v1 && v2 &&
            g.Get(rec + RecCursor, v.cursor_rec) && g.Get(rec + RecRequest, v.req))
            v.rec = rec;
    }
    // scene id and its attribute mask (HoldF2 pin)
    uint64_t mbase = 0;
    if (addr[ASceneId] && addr[AMaskGot] && g.Get(addr[ASceneId], v.scene) &&
        g.Get(addr[AMaskGot], mbase) && mbase) {
        const uint64_t ent =
            v.scene >= 0 && v.scene < 0x100 ? mbase + uint64_t(v.scene) * 8 : mbase;
        uint32_t m[2]{};
        if (v.scene == -1 || g.Read(ent, m, sizeof m)) {
            v.scene_mask = m[0] | uint64_t{m[1]} << 32;
            v.scene_ok = true;
        }
    }
    v.wrap_ok = addr[AWrapColor] && addr[AWrapTab1] && addr[AWrapTab4] &&
                g.Read(addr[AWrapColor], v.wrap_color, sizeof v.wrap_color) &&
                g.Read(addr[AWrapTab1], v.tab1, sizeof v.tab1) &&
                g.Read(addr[AWrapTab4], v.tab4, sizeof v.tab4);
    v.override_on = OverrideActive(g, v, s.player_no, v.override_ok) && v.override_ok;
    // pocket menu (PocketSlot / PocketVt / PocketSm / PocketCursor / PocketCmd pins)
    uint64_t root = 0, menu = 0, vt = 0;
    const uint64_t want_vt = PinData(g, PinPocketVt, 0, 1);
    if (ui_root && g.Get(ui_root, root) && IsHeap(root) && g.Get(root + 0x88, menu) &&
        IsHeap(menu) && g.Get(menu, vt) && vt == want_vt) {
        v.menu = menu;
        uint8_t cur = 0xff;
        if (g.Get(menu + MenuCursor, cur) && cur < 40)
            v.cursor = cur;
        int32_t id = -1;
        uint32_t count = 0;
        uint64_t names = 0, str = 0;
        if (g.Get(menu + MenuSm + 8, id) && g.Get(menu + MenuSm + 0x38, count) &&
            g.Get(menu + MenuSm + 0x40, names) && id >= 0 && static_cast<uint32_t>(id) < count &&
            count <= 64 && IsHeap(names) && g.Get(names + id * 16 + 8, str) && g.InMain(str, 1)) {
            char buf[32] = {};
            if (g.Read(str, buf, sizeof buf - 1))
                v.menu_state.assign(buf, strnlen(buf, sizeof buf - 1));
        }
        uint64_t win = 0;
        int32_t n = 0;
        if (g.Get(menu + MenuWindow, win) && IsHeap(win) && g.Get(win + WinCount, n) && n > 0 &&
            n <= 7 && g.Get(win + WinEntries + WinCmdId, v.win_first) &&
            g.Get(win + WinState, v.win_state)) {
            v.window = win;
            v.win_count = n;
        }
    }
    return true;
}

namespace {
/// BCSV row index of an in-memory row (the game's own computation, e.g. 0x19d79a4..0x19d79f8).
bool RowIndex(const live::Guest& g, uint64_t r, uint32_t& out) {
    uint32_t back = 0, rows = 0, len = 0;
    uint16_t cols = 0;
    uint8_t flag = 0;
    if (!r || !g.Get(r, back) || back == 0 || r == back)
        return false;
    const uint64_t hdr = r - back;
    if (!g.Get(hdr, rows) || !g.Get(hdr + 4, len) || !g.Get(hdr + 8, cols) ||
        !g.Get(hdr + 10, flag) || !rows || !len || !cols)
        return false;
    const uint64_t data = hdr + uint64_t{cols} * 8 + (flag == 0 ? 0xc : 0x1c);
    if (r < data)
        return false;
    const uint64_t k = (r - data) / len;
    if (k >= rows)
        return false;
    out = static_cast<uint32_t>(k);
    return true;
}
/// u16 [table+8][k] of a {u32 count, +8 u16*} table while k < count.
bool TableU16(const live::Guest& g, uint64_t table, uint32_t k, uint16_t& out) {
    uint32_t n = 0;
    uint64_t arr = 0;
    return table && g.Get(table, n) && k < n && g.Get(table + 8, arr) && arr &&
           g.Get(arr + uint64_t{k} * 2, out);
}
} // namespace

bool Writer::Facts(const live::Guest& g, const View& v, const Item8& it, ItemFacts& f,
                   std::string& why) {
    f = {};
    if (!v.wrap_ok) {
        why = "wrap table";
        return false;
    }
    // effective id (0x267d6c4) -> ItemParam row (ItemRow pin)
    const uint16_t id = EffectiveId(it, v.wrap_color, v.tab1, v.tab4);
    uint64_t gp = 0, t = 0, arr = 0, row = 0;
    uint16_t base = 0;
    uint32_t count = 0;
    if (!addr[AItemRowGot] || !g.Get(addr[AItemRowGot], gp) || !g.Get(gp, t) || !IsHeap(t) ||
        !g.Get(t + RowBase, base) || !g.Get(t + RowCount, count) || !g.Get(t + RowArray, arr)) {
        why = "item table";
        return false;
    }
    const uint32_t idx = static_cast<uint32_t>(id) - base;
    f.row = idx < count && g.Get(arr + uint64_t{idx} * 8, row) && row;
    if (!f.row)
        return true;
    uint32_t r = 0;
    const bool r_ok = RowIndex(g, row, r);
    // kind (HoldCmd pin: [GOT 0x4ac00c8] table)
    uint64_t ktab = 0;
    f.kind_known =
        r_ok && addr[AKindGot] && g.Get(addr[AKindGot], ktab) && TableU16(g, ktab, r, f.kind);
    // hold-class row (OutfitRow pin 0x19dab50): <= 0xa1, else 0; bad row 0
    uint64_t stab = 0;
    uint16_t outfit = 0;
    if (!addr[AOutfitGot] || !g.Get(addr[AOutfitGot], stab)) {
        why = "outfit table";
        return false;
    }
    if (!r_ok || !TableU16(g, stab, r, outfit) || outfit > 0xa1)
        outfit = 0;
    // outfit class (0x19d78cc; inline in F2 0x19dd978..0x19ddac4): <= 0x74, else 0
    {
        uint64_t ot = 0;
        int32_t n1 = 0;
        if (!addr[AOutfitCls] || !g.Get(addr[AOutfitCls], ot) || !IsHeap(ot) ||
            !g.Get(ot + 0x1c0, n1)) {
            why = "outfit class";
            return false;
        }
        uint64_t orow = 0;
        if (n1 >= 1) {
            uint32_t n0 = 0;
            uint64_t oarr = 0;
            if (static_cast<uint32_t>(n1) > outfit && g.Get(ot + 0x1b0, n0) &&
                g.Get(ot + 0x1b8, oarr) && oarr)
                g.Get(oarr + uint64_t{n0 > outfit ? outfit : 0u} * 8, orow);
        } else {
            // 0x19d7a2c: the table's own BCSV, row `outfit` (unless masked off at [+0xd0])
            uint64_t bc = 0, b = 0, mask = 0;
            uint32_t mn = 0, rows = 0, len = 0;
            uint16_t cols = 0;
            uint8_t flag = 0, m = 1;
            if (g.Get(ot + 0xb0, bc) && bc && g.Get(bc + 8, b) && b && g.Get(ot + 0xc8, mn) &&
                (mn <= outfit || (g.Get(ot + 0xd0, mask) && g.Get(mask + outfit, m) && m)) &&
                g.Get(b, rows) && rows > outfit && g.Get(b + 4, len) && len && g.Get(b + 8, cols) &&
                cols && g.Get(b + 10, flag))
                orow = b + uint64_t{cols} * 8 + (flag == 0 ? 0xc : 0x1c) + uint64_t{outfit} * len;
        }
        uint32_t k = 0;
        uint16_t c = 0;
        if (orow && RowIndex(g, orow, k) && TableU16(g, addr[AOutfitCls2], k, c) && c <= 0x74)
            f.cls = c;
    }
    // ItemOutfitCategory row of that class (0x19d81b0: class, 0 when none / out of range), column
    // Misc 0x42AD246A (u8) from the player's romfs: bit 4 = no Favorite (0x19dc088..0x19dc1a4)
    if (const Bcsv* cat_t = catalog ? catalog->Table("ItemOutfitCategory") : nullptr;
        cat_t && static_cast<size_t>(f.cls) < cat_t->Rows()) {
        const auto m = cat_t->Cell(static_cast<size_t>(f.cls), CategoryMiscKey);
        f.misc = m.empty() ? 0 : m[0];
    }
    // Storage (ItemOutfitInfo row `outfit`, column 0xC89FB7AF) from the player's romfs
    const Bcsv* table = catalog ? catalog->Table("ItemOutfitInfo") : nullptr;
    if (!table || outfit >= table->Rows()) {
        why = "outfit info";
        return false;
    }
    const auto cell = table->Cell(outfit, OutfitStorageKey);
    f.storage = cell.size() >= 2 ? static_cast<uint16_t>(cell[0] | cell[1] << 8) : 0;
    return true;
}

Writer::Verdict Writer::HoldOf(const live::Guest& g, const View& v, const Item8& it) {
    Verdict out;
    if (IsEmpty(it)) {
        out.hold = Hold::No;
        out.why = "empty slot";
        return out;
    }
    ItemFacts f;
    if (!v.scene_ok) {
        out.why = "scene";
        return out;
    }
    if (!Facts(g, v, it, f, out.why))
        return out; // Unknown: the drive asks the game's own menu
    return HoldFrom(v, f);
}

Writer::Verdict Writer::HoldFrom(const View& v, const ItemFacts& f) {
    Verdict out;
    HoldInputs in;
    in.row = f.row;
    in.kind_known = f.kind_known;
    in.kind = f.kind;
    in.class43 = f.cls == 0x43;
    in.storage = f.storage;
    in.scene = v.scene;
    in.scene_mask = v.scene_mask;
    out.hold = HoldRule(in);
    out.class43 = in.class43;
    out.why = out.hold == Hold::No        ? "can't hold that"
              : out.hold == Hold::Unknown ? "scene rule"
                                          : "";
    return out;
}

bool Writer::OverrideActive(const live::Guest& g, const View& v, int player_no, bool& ok) {
    // 0x2447c90: rec = [g]+0x284+no*0xc; active when g, no <= 7, byte +9 set, the item has an
    // ItemParam row and (hand-tool Storage or outfit class 0x1c). EquipTool then updates this
    // record instead of the ToolPack (0x2371f6c) and the Hold request is refused (0x2437490). Set
    // with flag 0x100 by 0xb515a0 (item 0xb0d) / 0xb7a440 and by EquipTool itself, cleared by
    // 0x241a33c.
    ok = false;
    uint64_t gp = 0;
    if (!addr[AOverride] || !g.Get(addr[AOverride], gp))
        return false;
    if (!gp || player_no < 0 || player_no > 7) {
        ok = true;
        return false;
    }
    uint8_t rec[12]{};
    if (!g.Read(gp + OverrideRec + uint64_t(player_no) * OverrideStride, rec, sizeof rec))
        return false;
    if (!rec[9]) {
        ok = true;
        return false;
    }
    Item8 it;
    std::memcpy(it.data(), rec, 8);
    ItemFacts f;
    std::string w;
    if (!Facts(g, v, it, f, w))
        return false; // unreadable: not ok -> gated (fail closed)
    ok = true;
    // live: {FE FF.., flags 0x100} after the NookPhone offline -> no row -> not active
    return f.row && (StorageIsHandTool(f.storage) || f.cls == 0x1c);
}

bool Writer::Apply(const View& v, const std::vector<Op>& ops) {
    if (ops.empty() || ops.size() > EDEN_DSMOD_WRITE_BATCH_MAX_OPS || !have_write ||
        !write.write_batch)
        return false;
    std::vector<EdenDsmodWriteOp> w(ops.size());
    for (size_t i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];
        const uint64_t base = op.base == Op::Personal ? v.per
                              : op.base == Op::Comp   ? v.comp
                                                      : v.rec;
        if (!base)
            return false;
        w[i].address = base + op.off;
        w[i].size = op.size;
        w[i].reserved = 0;
        w[i].expect = op.expect.data();
        w[i].value = op.value.data();
    }
    return write.write_batch(write.userdata, w.data(), static_cast<uint32_t>(w.size())) != 0;
}

bool Writer::ApplyAbs(uint64_t at, const uint8_t* expect, const uint8_t* value, uint8_t size) {
    if (!have_write || !write.write_batch || !at)
        return false;
    EdenDsmodWriteOp op{};
    op.address = at;
    op.size = size;
    op.reserved = 0;
    op.expect = expect;
    op.value = value;
    return write.write_batch(write.userdata, &op, 1) != 0;
}

void Writer::Finish(int r, std::string m) {
    result = r;
    msg = std::move(m);
    msg_ms = live::NowMs();
    if (r == 2 || r == 3) { // a refused / failed request: the card says so at once
        attempt_ms = msg_ms;
        dismissed = false;
    }
}

int Writer::CardCode(uint64_t now) const {
    if (DriveBusy()) // a request the game takes in about a second shows nothing; a longer drive the
                     // card
        return drive.start_ms && now - drive.start_ms >= CardAfterMs ? CardWorking : CardNone;
    const bool recent = attempt_ms && now - attempt_ms < CardAttemptMs && !dismissed;
    if (!why.empty()) {
        const int c = CardOf(why);
        // the game's own menu open (pocket menu, scene.bag): the page is covered at once, for as
        // long as it is open (owner 2026-10-02: the page does not follow the game's pocket cursor)
        if (recent || c == CardMenu)
            return c;
        // a lasting gate shows the card by itself (not the ones that never pass: runtime, version,
        // off)
        if (c != CardUnusable && gate_ms && now - gate_ms >= CardAfterMs)
            return c;
        return CardNone;
    }
    return recent && (result == 2 || result == 3) ? CardRefused : CardNone;
}

void Writer::PublishCard(const EdenDsmodHostApi& host, uint64_t now) {
    if (host.publish_i64) {
        host.publish_i64(host.userdata, "bag.card", CardCode(now));
        host.publish_i64(host.userdata, "bag.card.ring", card_why == "ring open" ? 1 : 0);
    }
}

void Writer::PublishNow() {
    if (!have_host)
        return;
    const EdenDsmodHostApi& host = host_copy;
    if (host.publish_text)
        host.publish_text(host.userdata, "bag.msg", msg.c_str());
    PublishCard(host, live::NowMs());
    if (host.publish_i64)
        host.publish_i64(host.userdata, "ring.open", ring_open ? 1 : 0);
}

bool Writer::Action(std::string_view name, int64_t arg, bool& handled) {
    handled = true;
    // an immediate refusal from the last sample's read: the runtime plays the refused haptic
    // instead of the drop's, and the note shows in this tick (the queued path re-checks with a
    // fresh read)
    auto refuse = [&](const std::string& code) {
        Finish(2, code.empty() ? "busy" : code); // gates open, only the drive running (DriveBusy)
        PublishNow();
        return false;
    };
    auto queue = [&](Kind k, int a, int b, const Item8& item = Item8{}) {
        if (req.kind != Kind::None || DriveBusy())
            return refuse("busy"); // one request at a time
        req = {k, a, b, item};
        return true;
    };
    if (name == "bag_writes") {
        enabled = arg != 0;
        return true;
    }
    if (name == "bag_card_tap") { // the refusal card tapped away (a lasting gate keeps it)
        dismissed = true;
        PublishNow();
        return true;
    }

    if (name == "bag_ring_close") {
        ring_open = false;
        PublishNow();
        return true;
    }
    if (name == "bag_star") {
        if (!can_write)
            return refuse(why);
        if (!ring_enable || !ring_scene)
            return refuse("no Favorite");
        if (arg >= 0 && (arg >= 40 || !slot_in[arg] || !slot_used[arg] || !slot_fav[arg]))
            return refuse("no Favorite");
        ring_open = true;
        ring_selected = -1;
        ring_center = -1;
        if (arg >= 0) {
            const auto it =
                std::find(ring_owners.begin(), ring_owners.end(), static_cast<int>(arg));
            if (it == ring_owners.end())
                ring_center = static_cast<int>(arg);
            else
                ring_selected = static_cast<int>(it - ring_owners.begin());
        }
        PublishNow();
        return true;
    }
    if (name == "bag_ring") { // a tap only selects; clear removes the highlighted favourite
        if (!ring_open)
            return refuse("ring closed");
        if (arg >= 0 && arg < RingPositions) {
            ring_selected = ring_owners[arg] >= 0 ? static_cast<int>(arg) : -1;
            PublishNow();
            return true;
        }
        if (arg != RingPositions || ring_selected < 0 || ring_owners[ring_selected] < 0)
            return refuse("no star");
        if (!can_write)
            return refuse(why);
        const int source = ring_owners[ring_selected];
        return queue(Kind::RingMove, source, -1, slot_item[source]);
    }
    if (name.starts_with("bag_ring_drop_")) {
        if (!ring_open)
            return refuse("ring closed");
        if (!can_write)
            return refuse(why);
        const std::string_view suffix = name.substr(14);
        if (suffix.size() != 1 || suffix[0] < '0' || suffix[0] > '8')
            return false;
        const int target = suffix[0] - '0';
        if (arg != ring_center &&
            std::find(ring_owners.begin(), ring_owners.end(), arg) == ring_owners.end())
            return refuse("item moved");
        if (arg < 0 || arg >= 40 || !slot_in[arg] || !slot_used[arg] || !slot_fav[arg])
            return refuse("no Favorite");
        return queue(Kind::RingMove, static_cast<int>(arg), target == 8 ? -1 : target,
                     slot_item[arg]);
    }
    if (name == "diy_fav") {
        // the DIY page's star (r9 polish): argument = recipe uid * 2 + the wanted state (1 = star,
        // 0 = clear), from diy.sel.favarg. The home grid allows this recipe-only write;
        // native app content retains the gate to avoid racing its cached favourite bits.
        if (!can_diy)
            return refuse(why);
        if (arg < 0 || arg >= 0x800 * 2)
            return refuse("no such recipe");
        return queue(Kind::RecipeFav, static_cast<int>(arg / 2), static_cast<int>(arg % 2));
    }
    if (name == "bag_move" || name.starts_with("bag_drop_")) {
        int from = -1, to = -1;
        // range-checked before the int narrowing (an s64 argument must not alias a valid slot)
        if (name == "bag_move") {
            if (arg < 0 || arg >= 64 * 40)
                return refuse("no such slot");
            from = static_cast<int>(arg / 64);
            to = static_cast<int>(arg % 64);
        } else {
            const std::string_view digits = name.substr(9);
            if (digits.empty() || digits.size() > 2)
                return false;
            for (char c : digits) {
                if (c < '0' || c > '9')
                    return false;
                to = (to < 0 ? 0 : to * 10) + (c - '0');
            }
            if (arg == 100) { // the equipped circle dragged out onto the bag
                if (!equipment_allowed)
                    return refuse("indoors");
                if (!can_write)
                    return refuse(why);
                if (linked < 0 && !tool_out)
                    return refuse("nothing held");
                return queue(Kind::Unequip, -1, -1);
            }
            if (arg < 0 || arg >= 40)
                return refuse("no such slot");
            from = static_cast<int>(arg);
        }
        // an item dropped back on its own slot: nothing to do -- accepted silently (no write, no
        // refusal card, no reject haptic), whatever the gates
        if (from == to && from >= 0 && from < 40)
            return true;
        if (!can_write)
            return refuse(why);
        if (from < 0 || from >= 40 || to < 0 || to >= 40 || !slot_in[from] || !slot_in[to])
            return refuse("no such slot");
        if (!slot_used[from] && !slot_used[to])
            return refuse("both empty");
        return queue(Kind::Move, from, to);
    }
    if (name == "bag_equip") {
        if (!equipment_allowed)
            return refuse("indoors");
        // the held item dropped back on the circle: already held, so nothing to do (silently)
        if (arg >= 0 && arg < 40 && arg == linked && tool_out)
            return true;
        if (!(can_equip || can_drive))
            return refuse(why.empty() ? std::string{"hands busy"} : why);
        if (arg < 0 || arg >= 40 || !slot_in[arg] || !slot_used[arg])
            return refuse("empty slot");
        if (slot_hold[arg] == Hold::No)
            return refuse("can't hold that");
        return queue(Kind::Equip, static_cast<int>(arg), -1);
    }
    if (name == "bag_unequip") {
        if (!equipment_allowed)
            return refuse("indoors");
        if (!can_write)
            return refuse(why);
        return queue(Kind::Unequip, -1, -1);
    }
    handled = false;
    return false;
}

void Writer::ReqFallback(const live::LiveSnapshot& s, const View& v) {
    if (!s.free || v.req != 0)
        return; // native tool changes and foreign requests must finish before any fallback

    std::vector<Op> ops;
    std::string w;
    const auto stop = [&](int r, const std::string& m) {
        drive = {};
        Finish(r, m);
    };
    if (drive.req_cls == 1) { // put away: EquipTool's stores with the empty item
        if (!BuildUnequip(v.p, v.comp_item, ops, w))
            stop(2, w);
        else if (Apply(v, ops))
            stop(1, "");
        else
            stop(3, "changed meanwhile");
        return;
    }
    const bool out = v.comp_ok && !IsEmpty(v.comp_item) && static_cast<int8_t>(v.p.link[1]) >= 0;
    if (out) { // a tool in the hand: the direct swap (EquipTool + SetItem stores)
        if (!BuildEquip(v.p, drive.target, v.comp_item, true, ops, w))
            stop(2, w);
        else if (Apply(v, ops))
            stop(1, "");
        else
            stop(3, "changed meanwhile");
        return;
    }
    drive.state =
        v.rec ? Drive::CycleSet : Drive::Open; // empty hands: the D-pad cycle, then the menu
    drive.settle = drive.waiting = 0;
}

void Writer::StartMenuDrive() {
    const int target = drive.target;
    const Item8 item = drive.item;
    const uint64_t start = drive.start_ms ? drive.start_ms : live::NowMs();
    drive = {};
    drive.state = Drive::Open;
    drive.target = target;
    drive.item = item;
    drive.start_ms = start;
}

void Writer::Sample(const EdenDsmodHostApi& host, const live::Guest& g, const live::LiveSnapshot& s,
                    uint64_t ui_root, bool page_open, bool phone_drive_busy, int selected) {
    host_copy = host;
    have_host = true;
    View v;
    const bool code = CheckPins(g);
    const bool view = code && s.personal_ok && ReadView(g, s, ui_root, v);

    // ---- gates (bag.can_write / bag.why) ----
    why.clear();
    if (!have_write)
        why = "no write_batch";
    else if (!enabled)
        why = "off";
    else if (!code)
        why = "code";
    else if (!view)
        why = "save";
    else if (!s.scene_ok || s.loading)
        why = "loading";
    else if (s.bag_open ||
             (!v.menu_state.empty() && v.menu_state != "cIdle" && v.menu_state != "cInit"))
        why = "menu open";
    // a ring menu (tools / reactions): the player's machine waits in cMenuWait (player state table
    // main+0x4808320 entry 25, the same wait the pocket menu uses) while the camera stays on the
    // field (the pocket menu moves it to cItemMenu: bag_open above). LIVE round 7: DUp -> cMenuWait
    // + cNormalField, R (reactions) -> the same, X -> cMenuWait + cItemMenu.
    else if (s.phone_open)
        why = "phone open";
    else if (s.player_ok && s.player.name == "cMenuWait")
        why = "ring open";
    else if (!v.session_ok || v.online)
        why = "online";
    else if (!v.override_ok || v.override_on)
        why = "override";
    else if (!s.free)
        why = "busy";
    else if (!v.comp_ok)
        why = "player";
    else if (phone_drive_busy)
        why = "phone drive";
    const bool gates = why.empty();
    const uint64_t now = live::NowMs();
    if (why != card_why) {
        gate_ms = gates ? 0 : now;
        attempt_ms = 0;
        dismissed = false;
    }
    if (gates)
        gate_ms = 0;
    else if (!gate_ms)
        gate_ms = now;
    card_why = why;
    can_write = gates && !DriveBusy();
    // The home grid has no recipe cache. Only RecipeFav can use this exception;
    // inventory operations retain their existing gate, and queued writes recheck it.
    const bool phone_home = s.phone_ok && s.phone.name == "cExecDevice" && s.camera_ok &&
                            s.camera.name == "cSmaphoMenu" && s.player_ok &&
                            s.player.name == "cMenuWait";
    can_diy = can_write ||
              (why == "phone open" && phone_home && v.session_ok && !v.online && v.override_ok &&
               !v.override_on && v.comp_ok && !phone_drive_busy && !DriveBusy());
    ring_enable = view && v.ring_flag > 0;
    // scene attribute bit 0x19 ([GOT 0x4ac39f8][scene s16], 0x307f8xx): the menu offers no Favorite
    // there; no scene (-1) is treated the same (conservative)
    ring_scene = view && v.scene_ok && v.scene >= 0 && ((v.scene_mask >> 0x19) & 1) == 0;
    tool_out = v.comp_ok && !IsEmpty(v.comp_item) && static_cast<int8_t>(v.p.link[1]) >= 0;
    equipment_allowed = !s.stage_name.empty() && live::SceneStructure(s.stage_name) == 0;
    can_equip = equipment_allowed && can_write && tool_out;
    can_drive = equipment_allowed && can_write && v.comp_ok;
    linked = view ? LinkedSlot(v.p) : -1;

    // ---- the Hold rule per slot (recomputed when the pockets or the scene change) ----
    if (view && (page_open || DriveBusy() || req.kind != Kind::None)) {
        uint64_t key = 0xcbf29ce484222325ULL;
        for (uint8_t c : v.p.raw)
            key = Mix(key, c);
        key = Mix(Mix(Mix(key, static_cast<uint16_t>(v.scene)), v.scene_mask), v.scene_ok ? 1 : 0);
        if (key != hold_key) {
            hold_key = key;
            bool unread = false;
            for (int i = 0; i < 40; ++i) {
                Slot sl;
                slot_in[i] = SlotOf(v.p, i, sl);
                const Item8 it = slot_in[i] ? v.p.ItemAt(sl.holder, sl.index) : EmptyItem;
                slot_item[i] = it;
                slot_used[i] = slot_in[i] && !IsEmpty(it);
                // one Facts read per slot: the Hold rule (HoldOf without the second read) and the
                // Favorite rule's item part (0x19dbe40(item, 1, 0, 0, 1))
                ItemFacts f;
                std::string w;
                const bool facts = slot_used[i] && Facts(g, v, it, f, w);
                unread = unread || (slot_used[i] && !facts);
                slot_hold[i] = !slot_used[i]         ? Hold::No
                               : facts && v.scene_ok ? HoldFrom(v, f).hold
                                                     : Hold::Unknown;
                slot_fav[i] = facts && f.row && FavItemRule(f.storage, f.misc);
                slot_cls[i] = !slot_used[i] ? 0 : facts ? (f.row ? f.cls : 0) : -1;
            }
            // a table the game had not built yet (boot) is read again next sample, not kept until
            // the pockets change
            if (unread)
                hold_key = 0;
        }
    } else if (!view) {
        slot_in.fill(false);
        slot_used.fill(false);
        slot_fav.fill(false);
        hold_key = 0;
    }

    // ---- one queued request: rebuilt from this sample's read, applied as one batch ----
    if (req.kind != Kind::None) {
        const Request r = req;
        req = {};
        std::vector<Op> ops;
        std::string w;
        if (!gates && !(r.kind == Kind::RecipeFav && can_diy)) {
            Finish(2, why);
        } else if (!equipment_allowed && (r.kind == Kind::Equip || r.kind == Kind::Unequip)) {
            Finish(2, "indoors");
        } else if (r.kind == Kind::RecipeFav) {
            // the DIY app's Favorite: the recipe's collect / favourite bytes read now, one op
            const unsigned uid = static_cast<unsigned>(r.a);
            uint8_t col = 0, fav = 0;
            if (!g.Get(v.per + RecipeCollectBits + (uid >> 3), col) ||
                !g.Get(v.per + RecipeFavBits + (uid >> 3), fav))
                Finish(3, "save");
            else if (!BuildRecipeFav(col, fav, uid, r.b != 0, ops, w))
                Finish(2, w);
            else if (ops.empty() || Apply(v, ops))
                Finish(1, "");
            else
                Finish(3, "changed meanwhile");
        } else if (r.kind == Kind::Fav || r.kind == Kind::RingMove) {
            // the ring: re-check the menu's Favorite offer with this sample's read, then the 1-2
            // SetFav stores (old owner of the position -> 0xFF, the slot -> position / 0xFF)
            Slot sl;
            const bool slot_ok = SlotOf(v.p, r.a, sl);
            const Item8 it = slot_ok ? v.p.ItemAt(sl.holder, sl.index) : EmptyItem;
            ItemFacts f;
            std::string fw;
            if (!slot_ok || IsEmpty(it)) {
                Finish(2, "empty slot");
            } else if (it != r.item) {
                Finish(2, "item moved"); // the slot changed between the picker tap and this read
            } else if ((!ring_enable || !ring_scene || !Facts(g, v, it, f, fw) || !f.row ||
                        !FavItemRule(f.storage, f.misc))) {
                Finish(2, "no Favorite");
            } else if (!(r.kind == Kind::RingMove ? BuildRingMove(v.p, r.a, r.b, ops, w)
                                                  : BuildFav(v.p, r.a, r.b, ops, w))) {
                Finish(2, w);
            } else if (ops.empty() || Apply(v, ops)) {
                if (r.kind == Kind::RingMove) {
                    const int from = RingPos(v.p.FavAt(sl.holder, sl.index));
                    const auto owners = RingOwners(v.p);
                    if (r.b < 0)
                        ring_center = r.a;
                    else if (from < 0)
                        ring_center = owners[r.b];
                    ring_selected = r.b;
                }
                Finish(1, "");
            } else {
                Finish(3, "changed meanwhile");
            }
        } else if (r.kind == Kind::Move) {
            if (!BuildMove(v.p, r.a, r.b, ops, w))
                Finish(2, w);
            else if (Apply(v, ops))
                Finish(1, "");
            else
                Finish(3, "changed meanwhile");
        } else if (r.kind == Kind::Unequip && can_drive && tool_out && v.rec && v.req == 0) {
            // primary: the game's own put-away (R+0x4E0 = 1, then L -> cToolChange), as native
            drive = {};
            drive.target = -1;
            drive.item = EmptyItem;
            drive.req_cls = 1;
            drive.start_ms = now;
            drive.state = Drive::ReqSet;
        } else if (r.kind == Kind::Unequip) {
            if (!BuildUnequip(v.p, v.comp_item, ops, w))
                Finish(2, w);
            else if (Apply(v, ops))
                Finish(1, "");
            else
                Finish(3, "changed meanwhile");
        } else if (r.kind == Kind::Equip) {
            Slot sl;
            const bool slot_ok = SlotOf(v.p, r.a, sl);
            const Item8 it = slot_ok ? v.p.ItemAt(sl.holder, sl.index) : EmptyItem;
            const Verdict vd = slot_ok ? HoldOf(g, v, it) : Verdict{};
            if (!slot_ok || IsEmpty(it)) {
                Finish(2, "empty slot");
            } else if (LinkedSlot(v.p) == r.a && tool_out) {
                Finish(2, "already held");
            } else if (vd.hold == Hold::No) {
                Finish(2, vd.why); // the game's pocket menu has no Hold for it (rule from code)
            } else if (int first = -1; vd.hold == Hold::Yes && !vd.class43 && can_drive && v.rec &&
                                       v.req == 0 && ReqClassFor(v.p, r.a, first) != 0) {
                // primary: the deferred tool request, taken by the game's own tool change on one L
                // press (equip from empty hands and swap). The game takes the FIRST pocket item of
                // the class: a later slot is swapped with that first one for the press and swapped
                // back after
                drive = {};
                drive.target = r.a;
                drive.item = it;
                drive.req_cls = static_cast<uint32_t>(slot_cls[r.a]);
                drive.req_first = first != r.a ? first : -1;
                drive.start_ms = now;
                drive.state = Drive::ReqSet;
            } else if (vd.hold == Hold::Yes && !vd.class43 && tool_out) {
                // the hand already shows a tool: the 3 stores of EquipTool + SetItem, instantly
                if (!BuildEquip(v.p, r.a, v.comp_item, true, ops, w))
                    Finish(2, w);
                else if (Apply(v, ops))
                    Finish(1, "");
                else
                    Finish(3, "changed meanwhile");
            } else if (can_drive) {
                drive = {};
                drive.target = r.a;
                drive.item = it;
                drive.start_ms = live::NowMs();
                // empty hands, a plain Hold item: the game's own D-pad tool cycle starting at our
                // slot (one press, no menu); class 0x43 / scene rule unknown: the pocket menu
                if (vd.hold == Hold::Yes && !vd.class43 && !tool_out && v.rec)
                    drive.state = Drive::CycleSet;
                else
                    drive.state = Drive::Open;
            } else {
                Finish(2, "hands busy");
            }
        }
    }

    if (!msg.empty() && !DriveBusy() && live::NowMs() - msg_ms > 5000)
        msg.clear(); // a refusal is shown for a few seconds
    // Keep the editor open across assignments and temporary game gates. Leaving Pockets closes it.
    if (!page_open)
        ring_open = false;
    if (ring_center >= 0 &&
        (!slot_in[ring_center] || !slot_used[ring_center] || !slot_fav[ring_center]))
        ring_center = -1;

    // ---- the empty-hands drive ----
    std::string press;
    if (DriveBusy()) {
        if (!drive.personal) {
            drive.personal = v.per;
            drive.actor = v.actor;
            drive.record = v.rec;
        }
        // The drive may open its own pocket menu or enter cToolChange; those are expected.
        // External phone, scene, session, override and write gates must still stop progression.
        const bool forward = have_write && enabled && code && view && s.scene_ok && !s.loading &&
                             equipment_allowed && v.session_ok && !v.online && v.override_ok &&
                             !v.override_on && v.comp_ok && !s.phone_open && !phone_drive_busy &&
                             (drive.state >= Drive::Open || !s.bag_open);
        StepDrive(g, s, v, view, forward, press);
    }

    // ---- publish ----
    auto I = [&](const char* n, int64_t x) {
        if (host.publish_i64)
            host.publish_i64(host.userdata, n, x);
    };
    auto T = [&](const char* n, const std::string& x) {
        if (host.publish_text)
            host.publish_text(host.userdata, n, x.c_str());
    };
    I("bag.can_write", can_write ? 1 : 0);
    I("diy.can_write", can_diy ? 1 : 0);
    const int native_app = s.phone_ui.ok && s.phone_ui.cursor >= 0 &&
                                   s.phone_ui.cursor < static_cast<int>(s.phone_ui.ids.size())
                               ? s.phone_ui.ids[s.phone_ui.cursor]
                               : -1;
    const bool native_diy =
        s.phone_open && s.phone.name == "cExecContent" && (native_app == 3 || native_app == 4);
    const int card = CardCode(now);
    I("diy.card", native_diy ? 2 : card == 3 ? 0 : card);
    T("bag.msg", msg);
    I("bag.card", CardCode(now));
    I("bag.card.ring", card_why == "ring open" ? 1 : 0);
    if (page_open || DriveBusy()) {
        // bag.{i}.hok: the held-item circle takes slot i (the manifest puts any other item's icon
        // in a group the circle refuses, so a drop that would be refused never plays the drop
        // haptic)
        const PubNames& pn = Names();
        for (int i = 0; i < 40; ++i) {
            const bool ok = slot_used[i] && slot_hold[i] != Hold::No && !(i == linked && tool_out);
            I(pn.hok[i].c_str(), ok ? 1 : 0);
        }
        // r9 (owner): the selected item is one the circle would refuse (the Hold rule above; the
        // item already held is not refused) -> the page dims the circle and its icon
        I("bag.sel.nohold",
          selected >= 0 && selected < 40 && slot_used[selected] && slot_hold[selected] == Hold::No
              ? 1
              : 0);
        const bool held = view && !IsEmpty(v.p.tool);
        I("equip.slot", held ? linked : -1);
        I("equip.has", held ? 1 : 0);
        I("equip.can", can_equip ? 1 : 0);
        I("equip.drive", can_drive ? 1 : 0);
        T("equip.icon",
          held ? "module:acnh/icon/item/" + std::to_string(ItemId(v.p.tool)) + "?s=104" : "");
        // the tool ring: bag.{i}.fok = the star button for slot i (the menu's Favorite offer: item
        // rule, ItemRingEnable, scene bit 0x19); ring.{p}.* = the 8 positions (clockwise from the
        // top) with their current tool; ring.open / .slot / .cur / .icon = the picker for the
        // selected slot
        for (int i = 0; i < 40; ++i)
            I(pn.fok[i].c_str(), slot_fav[i] && ring_enable && ring_scene ? 1 : 0);
        const auto own =
            view ? RingOwners(v.p) : std::array<int, RingPositions>{-1, -1, -1, -1, -1, -1, -1, -1};
        ring_owners = own;
        I("ring.available", ring_enable && ring_scene);
        for (int p = 0; p < RingPositions; ++p) {
            const int o = own[p];
            I(pn.ring_slot[p].c_str(), o);
            I(pn.ring_eq[p].c_str(), o >= 0 && o == linked && held ? 1 : 0);
            T(pn.ring_icon[p].c_str(),
              o >= 0 ? "module:acnh/icon/item/" + std::to_string(ItemId(slot_item[o])) + "?s=92"
                     : std::string{});
        }
        if (ring_selected >= 0 && own[ring_selected] < 0)
            ring_selected = -1;
        I("ring.open", ring_open);
        I("ring.cur", ring_selected);
        I("ring.center", ring_center);
        T("ring.icon",
          ring_center >= 0
              ? "module:acnh/icon/item/" + std::to_string(ItemId(slot_item[ring_center])) + "?s=120"
              : std::string{});
    }
    if (DriveBusy()) {
        // the shared button drive (manifest enforce, gate pdrv.pending): this sample's one press
        I("pdrv.pending", press.empty() ? 0 : 1);
        I("pdrv.idle", 0);
        for (const auto& [b, name] : PdrvButtons)
            I(name, press == b ? 1 : 0);
    }
}

// Empty hands. First the game's own D-pad tool cycle: its search starts AT the record's cursor
// (0x2480808..0x2480a38, hands empty), so writing the target's slot there and pressing DRight once
// makes the game pull that tool out itself (request 0x2435300 -> cToolPullout); when the target is
// not one the cycle takes, the game pulls out the next one and a direct swap (EquipTool's stores)
// finishes the Hold. No press taken / nothing cyclable -> the pocket menu: X opens it (player
// free), the D-pad moves its focus (re-planned from the live focus byte each settled step), A opens
// the item's command window, and A again only when that window's first command is Hold (command id
// 6). Done when the hand holds the item (comp+0xD4) and the ToolPack links the slot. One press per
// settled state, so a dropped press is simply retried; B backs out on any surprise.
void Writer::StepDrive(const live::Guest& g, const live::LiveSnapshot& s, const View& v,
                       bool view_ok, bool forward_allowed, std::string& press) {
    (void)g;
    constexpr int Settle = 3, Retry = 45, RetryOpen = 240,
                  CycleWaitMax = 150; // samples (~60 per second)
    constexpr uint64_t TimeoutMs = 25000;
    auto stop = [&](int r, const char* m) {
        drive = {};
        Finish(r, m);
    };
    // the any-slot request route's swap back (expect-guarded, one batch): taken -> swap back + the
    // tool cursor (= a native Hold of the target); not taken -> swap back + withdraw the request
    auto unswap = [&]() -> bool {
        if (!drive.req_swapped)
            return true;
        Slot target, first;
        if (!SlotOf(v.p, drive.target, target) || !SlotOf(v.p, drive.req_first, first) ||
            v.p.ItemAt(target.holder, target.index) != drive.displaced ||
            v.p.ItemAt(first.holder, first.index) != drive.item)
            return false; // native edits now own this order; never swap unrelated items
        std::vector<Op> ops;
        std::string w;
        const bool taken = v.req == 0 && v.comp_ok && v.comp_item == drive.item &&
                           v.p.tool == drive.item && LinkedSlot(v.p) == drive.req_first;
        if (taken) {
            // the tool cursor moved away from the first slot meanwhile (the player cycled tools):
            // swap back without touching it rather than retrying a batch that can never apply
            if (!BuildReqSwapBack(v.p, drive.target, drive.req_first, v.cursor_rec, ops, w) &&
                (w != "cursor" || !BuildMove(v.p, drive.target, drive.req_first, ops, w)))
                return false;
        } else {
            if (!BuildMove(v.p, drive.target, drive.req_first, ops, w))
                return false;
            if (v.req == drive.req_cls) {
                Op op;
                op.base = Op::Rec;
                op.off = RecRequest;
                op.size = 4;
                std::memcpy(op.expect.data(), &drive.req_cls, 4);
                ops.push_back(op); // value 0: withdraw
            }
        }
        if (!Apply(v, ops))
            return false;
        drive.req_swapped = false;
        if (!taken) {
            drive.req_first = -1;
            drive.req_withdrawn = true;
        }
        return true;
    };
    if (!view_ok) {
        // nothing to read, so nothing to write: a swapped route gets the same 5 s of grace as below
        // (the swapped pockets are a consistent order, item and link together), then the drive ends
        // instead of holding DriveBusy for good (title return, scene reload)
        const uint64_t age = live::NowMs() - drive.start_ms;
        if (age > TimeoutMs + (drive.req_swapped ? 5000 : 0))
            stop(3, "timeout");
        return;
    }
    if (v.per != drive.personal || v.actor != drive.actor || v.rec != drive.record) {
        stop(3, "player changed"); // old addresses belong to another scene/session; do not write
        return;
    }
    const uint64_t age = live::NowMs() - drive.start_ms;
    if (!forward_allowed || age > TimeoutMs || drive.steps > 60)
        drive.cancelled = true;
    if (drive.cancelled) {
        // Closing a gate retires only our own request and temporary swap. Never equip as fallback.
        if (drive.req_swapped && !unswap()) {
            if (live::NowMs() - drive.start_ms < TimeoutMs + 5000)
                return;
        }
        if (drive.req_owned && v.req == drive.req_cls && v.rec) {
            const uint32_t zero = 0;
            if (!ApplyAbs(v.rec + RecRequest, reinterpret_cast<const uint8_t*>(&drive.req_cls),
                          reinterpret_cast<const uint8_t*>(&zero), 4)) {
                if (age < TimeoutMs + 5000)
                    return;
                stop(3, "request changed");
                return;
            }
        }
        stop(2, age > TimeoutMs ? "timeout" : "equipment unavailable");
        return;
    }
    const std::string seen = v.menu_state + "|" + std::to_string(v.cursor) + "|" + s.player.name +
                             "|" + s.camera.name + "|" + std::to_string(v.win_count) + "|" +
                             std::to_string(v.win_state) + "|" +
                             std::to_string(ItemId(v.comp_item));
    if (seen != drive.seen) {
        drive.seen = seen;
        drive.settle = 0;
        drive.waiting = 0;
    }
    const bool menu_up = s.bag_open && v.menu && v.menu_state == "cPlay";
    const bool cmd_up = s.bag_open && v.menu && v.menu_state == "cCommandSelect" && v.window &&
                        v.win_count > 0 && v.win_state == WinOpen;
    // done: the hand shows the item and the ToolPack links the target
    const bool req_route = drive.state == Drive::ReqSet || drive.state == Drive::ReqPress ||
                           drive.state == Drive::ReqWait;
    if (v.comp_ok && v.comp_item == drive.item && LinkedSlot(v.p) == drive.target && !s.bag_open &&
        (!req_route || (v.req == 0 && !drive.req_swapped))) {
        stop(1, "");
        return;
    }
    if (drive.settle < Settle) {
        ++drive.settle;
        return;
    }
    Slot sl;
    const bool slot_ok =
        SlotOf(v.p, drive.target, sl) && v.p.ItemAt(sl.holder, sl.index) == drive.item;
    switch (drive.state) {
    case Drive::ReqSet: {
        if (drive.target >= 0 && !slot_ok) {
            stop(3, "item moved");
            return;
        }
        if (!s.free || !v.rec)
            return;
        if (v.req != 0) { // someone else's request pending: not ours to overwrite
            return; // a foreign pending operation owns the tool; wait without a fallback write
        }
        if (drive.req_first >= 0) {
            // a later slot of its class: swap it with the class's first slot and set the request in
            // one batch (the scan then takes our item from the first slot)
            int first = -1;
            std::vector<Op> ops;
            std::string w;
            if (ReqClassFor(v.p, drive.target, first) != drive.req_cls ||
                first != drive.req_first) {
                stop(3, "item moved");
                return;
            }
            if (!BuildReqSwap(v.p, drive.target, drive.req_first, drive.req_cls, ops, w)) {
                stop(2, w.c_str());
                return;
            }
            Slot first_slot;
            if (!SlotOf(v.p, drive.req_first, first_slot))
                return;
            drive.displaced = v.p.ItemAt(first_slot.holder, first_slot.index);
            if (!Apply(v, ops))
                return; // changed under us: read again
            drive.req_owned = true;
            drive.req_swapped = true;
            drive.state = Drive::ReqPress;
            return;
        }
        const uint32_t zero = 0;
        if (!ApplyAbs(v.rec + RecRequest, reinterpret_cast<const uint8_t*>(&zero),
                      reinterpret_cast<const uint8_t*>(&drive.req_cls), 4))
            return; // changed under us: read again
        drive.req_owned = true;
        drive.state = Drive::ReqPress;
        return;
    }
    case Drive::ReqPress:
        if (!s.free)
            return;
        press =
            "l"; // cEntryFixDirWalkAction -> cFixDirWalkWait -> ReturnToFree -> EnterWait reads it
        drive.state = Drive::ReqWait;
        ++drive.req_tries;
        drive.waiting = 0;
        break;
    case Drive::ReqWait:
        if (drive.req_withdrawn) {
            ReqFallback(s, v);
            return;
        }
        // done (the check above) when the hand shows the item and the ToolPack links the target /
        // is empty
        if (drive.req_swapped && v.req == 0 && v.comp_ok && v.comp_item == drive.item &&
            v.p.tool == drive.item && LinkedSlot(v.p) == drive.req_first) {
            // taken (EquipTool ran in cToolChange's enter): swap back, the link follows to the
            // target, and the tool cursor EquipTool stored for the first slot becomes the target's;
            // the done check finishes it next sample (a failed batch is read again next sample)
            unswap();
            return;
        }
        if (++drive.waiting <= ReqWaitMax)
            return;
        if (v.req == drive.req_cls && s.free && drive.req_tries < 2) {
            drive.state =
                Drive::ReqPress; // not taken (e.g. right after the tool ring closed): once more
            return;
        }
        if (drive.req_swapped) {
            // not taken: withdraw the request and swap back in one batch (expect-guarded: when the
            // game takes it meanwhile the batch fails and the taken branch runs next sample); the
            // fallback then reads the pockets again next sample
            unswap();
            return;
        }
        if (v.req == drive.req_cls) {
            const uint32_t zero = 0;
            if (!ApplyAbs(v.rec + RecRequest, reinterpret_cast<const uint8_t*>(&drive.req_cls),
                          reinterpret_cast<const uint8_t*>(&zero), 4))
                return; // taken meanwhile: the done check decides next sample
        }
        ReqFallback(s, v);
        return;
    case Drive::CycleSet: {
        if (!slot_ok) {
            stop(3, "item moved");
            return;
        }
        if (!s.free || !v.rec) {
            if (!v.rec)
                StartMenuDrive();
            return;
        }
        const int32_t want = Global(v.p, sl);
        if (v.cursor_rec != want) {
            uint8_t e[4], val[4];
            std::memcpy(e, &v.cursor_rec, 4);
            std::memcpy(val, &want, 4);
            if (!ApplyAbs(v.rec + RecCursor, e, val, 4))
                return; // changed under us: read again next sample
        }
        drive.state = Drive::CyclePress;
        return;
    }
    case Drive::CyclePress:
        if (!s.free)
            return;
        if (!IsEmpty(v.comp_item)) {
            drive.state = Drive::CycleWait; // something is already out
            return;
        }
        press =
            "right"; // pad trigger -> actor+0xed0 bit 5 -> 0x2223f64 -> the cycle from the cursor
        drive.state = Drive::CycleWait;
        ++drive.cycle_tries;
        drive.waiting = 0;
        break;
    case Drive::CycleWait:
        if (!IsEmpty(v.comp_item) && s.free) {
            // the cycle took another slot (the target is not one it takes): EquipTool's stores
            std::vector<Op> ops;
            std::string w;
            if (!slot_ok) {
                stop(3, "item moved");
            } else if (!BuildEquip(v.p, drive.target, v.comp_item, true, ops, w)) {
                stop(2, w.c_str());
            } else if (Apply(v, ops)) {
                stop(1, "");
            } else {
                stop(3, "changed meanwhile");
            }
            return;
        }
        if (IsEmpty(v.comp_item) && s.free && ++drive.waiting > CycleWaitMax) {
            // no tool came out: press once more, then the pocket menu
            if (drive.cycle_tries < 2)
                drive.state = Drive::CycleSet;
            else
                StartMenuDrive();
        }
        return;
    default:
        break;
    }
    if (drive.waiting) {
        // X gets a long grace (a second X while the menu is still opening would close it again)
        if (++drive.waiting < (drive.state == Drive::Open ? RetryOpen : Retry))
            return;        // a press is in flight
        drive.waiting = 0; // nothing changed: the press was dropped (e.g. the menu still appearing)
    }
    switch (drive.state) {
    case Drive::Open:
        if (menu_up) {
            drive.state = Drive::Cursor;
        } else if (!s.bag_open && s.free && drive.steps < 3) {
            press = "x"; // (again when a press was dropped)
        } else if (!s.bag_open && s.free) {
            stop(3, "menu did not open");
            return;
        }
        break;
    case Drive::Cursor:
        if (!menu_up) {
            if (cmd_up)
                press = "b"; // a command window we did not ask for
            else if (!s.bag_open)
                stop(3, "menu closed");
            break;
        }
        if (v.cursor == drive.target) {
            // the item there must still be the one we are after
            if (!slot_ok) {
                press = "b";
                drive.state = Drive::Wait;
                Finish(3, "item moved");
                break;
            }
            press = "a";
            drive.state = Drive::Submenu;
        } else if (v.cursor < 0) {
            press = "right"; // no focus yet: any move gives one
        } else {
            const int tr = drive.target / MenuCols, tc = drive.target % MenuCols;
            const int cr = v.cursor / MenuCols, cc = v.cursor % MenuCols;
            press = tr != cr ? (tr > cr ? "down" : "up") : (tc > cc ? "right" : "left");
        }
        break;
    case Drive::Submenu:
        if (cmd_up) {
            if (v.win_first == CmdHold && v.cursor == drive.target) {
                press = "a"; // Hold (the window opens with its first command under the cursor)
                drive.state = Drive::Hold;
            } else {
                press = "b";
                drive.state = Drive::Wait;
                Finish(3, "no Hold command");
            }
        } else if (menu_up) {
            drive.state = Drive::Cursor; // the A was dropped: plan again
        }
        break;
    case Drive::Hold:
        // the menu closes and the player changes tools (cToolChange); the done check above ends it
        if (cmd_up)
            press = "a";
        break;
    case Drive::Wait:
        // backing out after a refusal: close everything, then stop (result already set)
        if (cmd_up || menu_up) {
            press = "b";
        } else if (!s.bag_open) {
            drive = {};
            return;
        }
        break;
    default:
        break;
    }
    if (!press.empty()) {
        ++drive.steps;
        drive.waiting = 1;
    }
}

} // namespace acnh::bag
