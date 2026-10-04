// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH Bag page actions: the pure recipes (acnh_bag.h). The live writer is acnh_bag_live.cpp.

#include "acnh_bag.h"

#include <algorithm>
#include <cstring>

namespace acnh::bag {

bool Pockets::FromPersonal(const uint8_t* per, size_t size) {
    if (!per || size < ItemBag + Span || size < ToolLink + 2)
        return false;
    std::memcpy(raw.data(), per + ItemBag, Span);
    std::memcpy(tool.data(), per + ToolItem, 8);
    std::memcpy(link.data(), per + ToolLink, 2);
    return true;
}

uint32_t Pockets::Count(int holder) const {
    uint32_t n;
    std::memcpy(&n, raw.data() + (HolderBase(holder) - ItemBag) + HolderCount, 4);
    return n;
}

Item8 Pockets::ItemAt(int holder, int index) const {
    Item8 it;
    std::memcpy(it.data(), raw.data() + (HolderBase(holder) - ItemBag) + index * 8, 8);
    return it;
}

uint8_t Pockets::FavAt(int holder, int index) const {
    return raw[(HolderBase(holder) - ItemBag) + HolderFav + index];
}

bool SlotOf(const Pockets& p, int display, Slot& out) {
    if (display < 0 || display >= 40)
        return false;
    Slot s;
    s.holder = display < 20 ? 0 : 1;
    s.index = display < 20 ? display : display - 20;
    // SwapSlots bounds: an index at or past min(count, 20) of its holder is not a slot.
    if (static_cast<uint32_t>(s.index) >= std::min<uint32_t>(p.Count(s.holder), 20))
        return false;
    out = s;
    return true;
}

int LinkedSlot(const Pockets& p) {
    const int holder = p.link[0], index = static_cast<int8_t>(p.link[1]);
    if (index < 0 || index >= 20 || holder > 1)
        return -1;
    return holder == 0 ? index : 20 + index;
}

namespace {
Op Personal(uint32_t off, const uint8_t* expect, const uint8_t* value, uint8_t size) {
    Op op;
    op.base = Op::Personal;
    op.off = off;
    op.size = size;
    std::memcpy(op.expect.data(), expect, size);
    std::memcpy(op.value.data(), value, size);
    return op;
}
Op Comp(const Item8& expect, const Item8& value) {
    Op op;
    op.base = Op::Comp;
    op.off = CompItem;
    op.size = 8;
    op.expect = expect;
    op.value = value;
    return op;
}
} // namespace

bool BuildMove(const Pockets& p, int a, int b, std::vector<Op>& ops, std::string& why) {
    ops.clear();
    Slot sa, sb;
    if (!SlotOf(p, a, sa) || !SlotOf(p, b, sb)) {
        why = "no such slot";
        return false;
    }
    if (a == b) {
        why = "same slot";
        return false;
    }
    const Item8 ia = p.ItemAt(sa.holder, sa.index), ib = p.ItemAt(sb.holder, sb.index);
    if (IsEmpty(ia) && IsEmpty(ib)) {
        why = "both empty";
        return false;
    }
    const uint8_t fa = p.FavAt(sa.holder, sa.index), fb = p.FavAt(sb.holder, sb.index);
    // SwapSlots: SetSlot(a, item b) (+fav a := fav b), SetSlot(b, item a) (+fav b := fav a)
    ops.push_back(Personal(sa.ItemOff(), ia.data(), ib.data(), 8));
    ops.push_back(Personal(sb.ItemOff(), ib.data(), ia.data(), 8));
    ops.push_back(Personal(sa.FavOff(), &fa, &fb, 1));
    ops.push_back(Personal(sb.FavOff(), &fb, &fa, 1));
    // relink 0x2716c30: the ToolPack follows the held item (same item bytes, new holder/index)
    const int linked = LinkedSlot(p);
    if (linked == a || linked == b) {
        const Slot& to = linked == a ? sb : sa;
        const uint8_t now[2] = {p.link[0], p.link[1]};
        const uint8_t next[2] = {static_cast<uint8_t>(to.holder), static_cast<uint8_t>(to.index)};
        ops.push_back(Personal(ToolLink, now, next, 2));
    }
    return true;
}

namespace {
Op Record(uint32_t off, const uint8_t* expect, const uint8_t* value, uint8_t size) {
    Op op;
    op.base = Op::Rec;
    op.off = off;
    op.size = size;
    std::memcpy(op.expect.data(), expect, size);
    std::memcpy(op.value.data(), value, size);
    return op;
}
} // namespace

int32_t ToolCursorOf(const Slot& sl) {
    return sl.holder == 0 ? sl.index : sl.index + 0x14; // 0x2716bd8..0x2716c00
}

bool BuildReqSwap(const Pockets& p, int s, int f, uint32_t cls, std::vector<Op>& ops,
                  std::string& why) {
    ops.clear();
    if (cls < 2) {
        why = "no class";
        return false;
    }
    if (!BuildMove(p, s, f, ops, why))
        return false;
    const uint32_t zero = 0;
    ops.push_back(Record(RecRequestOff, reinterpret_cast<const uint8_t*>(&zero),
                         reinterpret_cast<const uint8_t*>(&cls), 4));
    return true;
}

bool BuildReqSwapBack(const Pockets& p, int s, int f, int32_t cursor_now, std::vector<Op>& ops,
                      std::string& why) {
    ops.clear();
    Slot ss, sf;
    if (!SlotOf(p, s, ss) || !SlotOf(p, f, sf)) {
        why = "no such slot";
        return false;
    }
    if (LinkedSlot(p) != f) {
        why = "not taken";
        return false;
    }
    if (cursor_now != ToolCursorOf(sf)) {
        why = "cursor";
        return false;
    }
    if (!BuildMove(p, s, f, ops, why))
        return false;
    const int32_t want = ToolCursorOf(ss);
    ops.push_back(Record(RecCursorOff, reinterpret_cast<const uint8_t*>(&cursor_now),
                         reinterpret_cast<const uint8_t*>(&want), 4));
    return true;
}

bool BuildEquip(const Pockets& p, int s, const Item8& comp_item, bool holdable,
                std::vector<Op>& ops, std::string& why) {
    ops.clear();
    Slot sl;
    if (!SlotOf(p, s, sl)) {
        why = "no such slot";
        return false;
    }
    const Item8 it = p.ItemAt(sl.holder, sl.index);
    if (IsEmpty(it)) {
        why = "empty slot";
        return false;
    }
    if (!holdable) {
        why = "can't hold that";
        return false;
    }
    if (LinkedSlot(p) == s) {
        why = "already held";
        return false;
    }
    if (IsEmpty(comp_item) || static_cast<int8_t>(p.link[1]) < 0) {
        why = "hands empty";
        return false;
    }
    const uint8_t link[2] = {static_cast<uint8_t>(sl.holder), static_cast<uint8_t>(sl.index)};
    ops.push_back(Personal(ToolItem, p.tool.data(), it.data(), 8));
    ops.push_back(Personal(ToolLink, p.link.data(), link, 2));
    ops.push_back(Comp(comp_item, it));
    return true;
}

bool BuildUnequip(const Pockets& p, const Item8& comp_item, std::vector<Op>& ops,
                  std::string& why) {
    ops.clear();
    if (IsEmpty(p.tool) && static_cast<int8_t>(p.link[1]) < 0 && IsEmpty(comp_item)) {
        why = "nothing held";
        return false;
    }
    const uint8_t link[2] = {0x00, 0xFF};
    ops.push_back(Personal(ToolItem, p.tool.data(), EmptyItem.data(), 8));
    ops.push_back(Personal(ToolLink, p.link.data(), link, 2));
    ops.push_back(Comp(comp_item, EmptyItem));
    return true;
}

std::array<int, RingPositions> RingOwners(const Pockets& p, int* dups) {
    std::array<int, RingPositions> own;
    own.fill(-1);
    int d = 0;
    for (int i = 0; i < 40; ++i) {
        Slot sl;
        if (!SlotOf(p, i, sl))
            continue;
        const int pos = RingPos(p.FavAt(sl.holder, sl.index));
        if (pos < 0)
            continue;
        if (own[pos] >= 0)
            ++d;
        else
            own[pos] = i;
    }
    if (dups)
        *dups = d;
    return own;
}

bool BuildFav(const Pockets& p, int s, int pos, std::vector<Op>& ops, std::string& why) {
    ops.clear();
    Slot sl;
    if (!SlotOf(p, s, sl)) {
        why = "no such slot";
        return false;
    }
    if (IsEmpty(p.ItemAt(sl.holder, sl.index))) {
        why = "empty slot";
        return false;
    }
    if (pos < -1 || pos >= RingPositions) {
        why = "no such position";
        return false;
    }
    const uint8_t cur = p.FavAt(sl.holder, sl.index);
    const uint8_t want = pos < 0 ? NoFav : static_cast<uint8_t>(pos);
    if (cur == want) {
        why = pos < 0 ? "no star" : "already there";
        return false;
    }
    int dups = 0;
    const auto own = RingOwners(p, &dups);
    if (dups) {
        why = "ring changed"; // two slots on one position: never the game's state, write nothing
        return false;
    }
    // 0x30e6da8: the chosen position's old owner loses its star, then 0x30e6df8 the new slot's byte
    if (pos >= 0 && own[pos] >= 0 && own[pos] != s) {
        Slot ol;
        SlotOf(p, own[pos], ol);
        const uint8_t old = static_cast<uint8_t>(pos);
        ops.push_back(Personal(ol.FavOff(), &old, &NoFav, 1));
    }
    ops.push_back(Personal(sl.FavOff(), &cur, &want, 1));
    return true;
}

bool BuildRingMove(const Pockets& p, int s, int pos, std::vector<Op>& ops, std::string& why) {
    ops.clear();
    Slot sl;
    if (!SlotOf(p, s, sl)) {
        why = "no such slot";
        return false;
    }
    if (IsEmpty(p.ItemAt(sl.holder, sl.index))) {
        why = "empty slot";
        return false;
    }
    const int from = RingPos(p.FavAt(sl.holder, sl.index));
    int dups = 0;
    const auto owners = RingOwners(p, &dups);
    if (dups) {
        why = "ring changed";
        return false;
    }
    if (pos == from) {
        ops.clear();
        return true;
    }
    if (from < 0 || pos < 0 || pos >= RingPositions || owners[pos] < 0)
        return BuildFav(p, s, pos, ops, why);
    Slot target;
    SlotOf(p, owners[pos], target);
    const uint8_t old = static_cast<uint8_t>(from), next = static_cast<uint8_t>(pos);
    ops = {Personal(sl.FavOff(), &old, &next, 1), Personal(target.FavOff(), &next, &old, 1)};
    return true;
}

bool FavItemRule(uint16_t storage, uint8_t category_misc) {
    return StorageIsHandTool(storage) && (category_misc & 0x10) == 0;
}

bool ApplyToImage(const std::vector<Op>& ops, std::vector<uint8_t>& personal, Item8* comp,
                  std::vector<uint8_t>* rec) {
    for (const Op& op : ops) {
        const uint8_t* at = nullptr;
        if (op.base == Op::Personal) {
            if (op.off + op.size > personal.size())
                return false;
            at = personal.data() + op.off;
        } else if (op.base == Op::Rec) {
            if (!rec || op.off + op.size > rec->size())
                return false;
            at = rec->data() + op.off;
        } else {
            if (!comp || op.off != CompItem || op.size != 8)
                return false;
            at = comp->data();
        }
        if (std::memcmp(at, op.expect.data(), op.size) != 0)
            return false;
    }
    for (const Op& op : ops) {
        if (op.base == Op::Personal)
            std::memcpy(personal.data() + op.off, op.value.data(), op.size);
        else if (op.base == Op::Rec)
            std::memcpy(rec->data() + op.off, op.value.data(), op.size);
        else
            std::memcpy(comp->data(), op.value.data(), op.size);
    }
    return true;
}

bool StorageIsHandTool(uint16_t storage) {
    for (int bit = 1; bit <= 13; ++bit)
        if (storage >> bit & 1)
            return bit == 9;
    return false;
}

bool KindAllowsHold(uint16_t kind) {
    // 0x19dc6c8..0x19dc6e4: kind > 0x20, or bit `kind` of 0x40ffffff
    return kind > 0x20 || (uint64_t{0x40ffffff} >> kind & 1u) != 0; // 64-bit lsr: kind 0x20 -> 0
}

Hold HoldRule(const HoldInputs& in) {
    // 0x3088df0: no ItemParam row for the effective id -> the command list stays empty
    if (!in.row)
        return Hold::No;
    // 0x307fb08: kind 0x78 never gets Hold (F1 0x19dddf0 takes those to "Build Fence"); an
    // unreadable kind row skips this test (0x307fb94)
    if (in.kind_known && in.kind == 0x78)
        return Hold::No;
    // F2 0x19dd954(item, {1, 0, 0, null})
    if (in.class43)
        return Hold::Yes; // 0x19ddac4: outfit class 0x43, no further test
    if (in.scene < 0)
        return Hold::No; // 0x19ddb60: no scene
    const auto bit = [&](int b) { return (in.scene_mask >> b & 1) != 0; };
    if (bit(5) || bit(6) || bit(7) || bit(0))
        return Hold::Unknown; // 0x19dc980 / 0x19dce10: other scenes' rules, not reproduced
    if (!bit(1))
        return Hold::No;                             // 0x19ddcc4
    const bool hand = StorageIsHandTool(in.storage); // 0x19dba10(item, 1) -> 0x19dab50
    if (bit(0x17)) {
        // 0x19dc580: hand tool and the kind test (kind unreadable -> 0, > 0xbc -> 0)
        const uint16_t k = in.kind_known && in.kind <= 0xbc ? in.kind : 0;
        if (!(hand && KindAllowsHold(k)))
            return Hold::No;
    }
    return hand ? Hold::Yes : Hold::No;
}

uint16_t EffectiveId(const Item8& it, const uint32_t* wrap_color, const uint32_t* tab1,
                     const uint32_t* tab4) {
    const uint16_t id = ItemId(it);
    if (id == 0x16a1 || id == 0x3100)
        return id;
    const uint8_t b3 = it[3];
    const uint32_t v = b3 >> 2 & 0xf;
    switch (b3 & 3) {
    case 0:
        return id;
    case 1:
        return static_cast<uint16_t>(tab1[v < 0x11 ? v : 0]);
    case 2:
        return 0x1180;
    default:
        if (v == 0xf)
            return static_cast<uint16_t>(tab1[0x10]);
        for (uint32_t k = 1; k < 4; ++k) // WrapColor 0x1a03610
            if (wrap_color[k] == v)
                return static_cast<uint16_t>(tab4[k]);
        return v == 0xb ? 0x3a19 : v == 0xa ? 0x306 : 0x1225;
    }
}

bool BuildRecipeFav(uint8_t collect_byte, uint8_t fav_byte, unsigned uid, bool want,
                    std::vector<Op>& ops, std::string& why) {
    ops.clear();
    if (uid >= 0x800) { // RecipeFavOn / Off store nothing for uid > 0x7ff (cmp #0x7ff; b.hi)
        why = "no such recipe";
        return false;
    }
    const uint8_t mask = static_cast<uint8_t>(1u << (uid & 7));
    if (!(collect_byte & mask)) { // the app lists known recipes only
        why = "unknown recipe";
        return false;
    }
    if (((fav_byte & mask) != 0) == want)
        return true;
    Op op;
    op.base = Op::Personal;
    op.off = RecipeFavBits + (uid >> 3);
    op.size = 1;
    op.expect[0] = fav_byte;
    op.value[0] = static_cast<uint8_t>(want ? fav_byte | mask : fav_byte & ~mask);
    ops.push_back(op);
    return true;
}

} // namespace acnh::bag
