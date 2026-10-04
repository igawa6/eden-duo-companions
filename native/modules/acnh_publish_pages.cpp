// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH companion publishing: the Pockets, Critterpedia and DIY pages (acnh_publish.h).

#include "acnh_publish.h"
#include "acnh_publish_detail.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "acnh_bcsv.h"
#include "acnh_catalog.h"
#include "acnh_lang.h"
#include "acnh_lang_grammar.h"
#include "acnh_map.h"
#include "acnh_msbt.h"
#include "acnh_romfs.h"

namespace acnh {
using namespace pub;
namespace {

/// Minimal BYML (v2-v4 "YB") walk for the PictureBook files: root dict -> first array -> per
/// element dict -> first int value. False on anything unexpected.
bool BookByml(const std::vector<uint8_t>& b, std::vector<uint16_t>& out) {
    out.clear();
    const auto u8 = [&](size_t o) -> int { return o < b.size() ? b[o] : -1; };
    const auto u24 = [&](size_t o) -> int64_t {
        return o + 3 <= b.size() ? int64_t{b[o]} | int64_t{b[o + 1]} << 8 | int64_t{b[o + 2]} << 16
                                 : -1;
    };
    const auto u32 = [&](size_t o) -> int64_t {
        if (o + 4 > b.size())
            return -1;
        uint32_t v;
        std::memcpy(&v, b.data() + o, 4);
        return v;
    };
    if (b.size() < 16 || b[0] != 'Y' || b[1] != 'B' || u8(2) < 2 || u8(2) > 4)
        return false;
    const int64_t root = u32(0xC);
    if (root <= 0 || u8(root) != 0xC1 || u24(root + 1) < 1)
        return false;
    int64_t arr = -1;
    for (int64_t i = 0, n = u24(root + 1); i < n && arr < 0; ++i)
        if (u8(root + 4 + i * 8 + 3) == 0xC0)
            arr = u32(root + 4 + i * 8 + 4);
    if (arr <= 0 || u8(arr) != 0xC0)
        return false;
    const int64_t n = u24(arr + 1);
    const int64_t vals = arr + 4 + ((n + 3) & ~int64_t{3});
    for (int64_t i = 0; i < n; ++i) {
        if (u8(arr + 4 + i) != 0xC1)
            return false;
        const int64_t d = u32(vals + i * 4);
        if (d <= 0 || u8(d) != 0xC1 || u24(d + 1) < 1)
            return false;
        const int t = u8(d + 4 + 3);
        const int64_t v = u32(d + 4 + 4);
        if ((t != 0xD1 && t != 0xD3) || v < 0 || v > 0xFFFF)
            return false;
        out.push_back(static_cast<uint16_t>(v));
    }
    return !out.empty();
}

// DIY categories. The icon index is core's RecipeCatIcons (= gen_manifest.py RECIPE_CATS):
// 0 All 1 Tool 2 Furniture 3 Goods 4 WallMount 5 Ceiling 6 FloorWall 7 Ring 8 GoodsBag 9 Food
// 10 Dessert 11 Season 12 CanMake 13 CanMakeBoth 14 Favorite 15 Request 16 TradeBox.
// UI categories each icon stands for (ItemParam.ItemUICategory; the category predicates test the
// result item's enum: 0x49e85xx..): used for the detail panel's category square.
const std::vector<std::vector<std::string_view>> DiyCatUi = {
    {},
    {"Tool"},
    {"Floor"},
    {"Upper"},
    {"Wall"},
    {"Ceiling"},
    {"RoomWall", "RoomFloor", "Ceiling_Rug"},
    {"Clothes"},
    {"Others"},
    {"Food"},
    {"Desert"}};
// The DIY app's own category list, in its order (MenuRecipe table main+0x55cb9c8, 17 x 0x70,
// static init 0x2e94240; enable rules from the jump table 0x4081598 at 0x2e9e448): label
// (String/STR_CategoryName), icon, rule.
enum class DiyRule {
    All,
    Requests,
    Seasonal,
    Ui,
    Other,
    Cooking,
    Craftable,
    CraftableBoth,
    Favorites,
    DiyRequests
};
struct DiyCatDef {
    const char* label;
    int icon;
    DiyRule rule;
};
const DiyCatDef DiyCats[] = {{"1801", 0, DiyRule::All},
                             {"1819", 15, DiyRule::Requests},
                             {"1814", 11, DiyRule::Seasonal},
                             {"1806", 1, DiyRule::Ui},
                             {"1807", 2, DiyRule::Ui},
                             {"1808", 3, DiyRule::Ui},
                             {"1809", 4, DiyRule::Ui},
                             {"1817", 5, DiyRule::Ui},
                             {"1813", 6, DiyRule::Ui},
                             {"1812", 7, DiyRule::Ui},
                             {"1811", 8, DiyRule::Other},
                             {"1815", 9, DiyRule::Cooking},
                             {"1816", 10, DiyRule::Cooking},
                             {"1802", 12, DiyRule::Craftable},
                             {"1802", 13, DiyRule::CraftableBoth},
                             {"1803", 14, DiyRule::Favorites},
                             {"1818", 16, DiyRule::DiyRequests}};

/// Hours (bit h = clock hour h) a critter appears in month `mo` (1-12) in hemisphere `hemi`: any
/// non-zero slot of its appear-table row(s). Slots -> hour pieces of the Critterpedia detail bar:
/// insects Morning 4-8, Daytime 8-16, Evening1 16-17, Evening2 17-19, Night 19-23, Midnight 23-4;
/// fish / sea creatures MorningAndEvening 4-9 + 16-21, Daytime 9-16, Night 21-4.
uint32_t ActiveHours(Catalog& cat, int kind, uint16_t item, int mo, int hemi) {
    static const char* const InsectMonth[] = {"January",   "Februay", "March",    "April",
                                              "May",       "June",    "July",     "August",
                                              "September", "October", "November", "December"};
    static const char* const Short[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const auto span = [](int a, int b) { return ((1u << b) - 1) & ~((1u << a) - 1); };
    struct Slot {
        const char* name;
        uint32_t hours;
    };
    const Slot Ins[] = {{"Morning", span(4, 8)},    {"Daytime", span(8, 16)},
                        {"Evening1", span(16, 17)}, {"Evening2", span(17, 19)},
                        {"Night", span(19, 23)},    {"Midnight", span(23, 24) | span(0, 4)}};
    const Slot Fsh[] = {{"MorningAndEvening", span(4, 9) | span(16, 21)},
                        {"Daytime", span(9, 16)},
                        {"Night", span(21, 24) | span(0, 4)}};
    if (mo < 1 || mo > 12)
        return 0;
    uint32_t out = 0;
    const auto scan = [&](const char* table, const Slot* slots, size_t n,
                          const std::string& month) {
        const Bcsv* t = cat.Table(table);
        if (!t)
            return;
        const uint32_t area_u8 = Bcsv::Key("AppearArea", "u8"), kitem = Bcsv::Key("ItemID", "u16");
        for (size_t r = 0; r < t->Rows(); ++r) {
            if (t->U(r, kitem) != item)
                continue;
            if (t->Has(area_u8) ? static_cast<int>(t->U(r, area_u8)) != hemi
                                : (t->Has(0x64330cb0) &&
                                   t->U(r, 0x64330cb0) != Crc32(hemi ? "South" : "North")))
                continue;
            for (size_t i = 0; i < n; ++i) {
                const uint32_t kc = Bcsv::Key("Prob" + month + slots[i].name, "u16");
                if (t->Has(kc) && t->U(r, kc) > 0)
                    out |= slots[i].hours;
            }
        }
    };
    if (kind == 0) {
        scan("InsectAppearParam", Ins, std::size(Ins), InsectMonth[mo - 1]);
    } else if (kind == 1) {
        scan("FishAppearRiverParam", Fsh, std::size(Fsh), Short[mo - 1]);
        scan("FishAppearSeaParam", Fsh, std::size(Fsh), Short[mo - 1]);
    } else {
        scan("SeafoodAppearParam", Fsh, std::size(Fsh), Short[mo - 1]);
    }
    return out;
}

} // namespace

// ---- bag page --------------------------------------------------------------------------------
void Publisher::BuildBag(const live::LiveSnapshot& s) {
    Catalog* cat = services.catalog;
    uint64_t key = Mix(0xba9, static_cast<uint64_t>(lang + 1));
    key = MixItems(key, s.pockets);
    key = Mix(key, static_cast<uint64_t>(s.pocket_slots));
    key = Mix(key, static_cast<uint64_t>(s.miles + 2));
    key = Mix(key, static_cast<uint64_t>(s.bank + 2));
    // the Pockets page keeps its own selection: while the game's pocket menu is open the page is
    // covered by the blocking card anyway (owner 2026-10-02: no following the game's cursor)
    key = Mix(key, static_cast<uint64_t>(bag_sel + 1));
    key = Mix(key, s.personal_ok);
    key = Mix(key, s.case_table != nullptr);
    if (o_bag.key == key)
        return;
    o_bag.Clear();
    o_bag.key = key;
    if (!s.personal_ok || !cat)
        return;
    const int slots = s.pocket_slots;
    int used = 0;
    const Lang l = static_cast<Lang>(std::max(lang, 0));
    std::array<std::string, 40> names;
    for (int i = 0; i < 40; ++i) {
        const std::string p = "bag." + std::to_string(i) + ".";
        const live::Item& it = s.pockets[i];
        const bool in = i < slots;
        const int count = in ? StackCount(*cat, it) : 0;
        o_bag.I(p + "count", count);
        o_bag.I(p + "fav", in && !it.Empty() && it.fav >= 0 ? 1 : 0);
        // every slot publishes every name ("" = nothing to draw), so no stale value can show
        if (count > 0)
            ++used;
        o_bag.T(p + "icon", count > 0
                                ? std::string{Key} + "icon/item/" + std::to_string(it.id) + "?s=101"
                                : "");
        // the pocket balloon: LayoutMsg/MainScreen 0101 ("{50:3}{item}": "Golden shovel"); a stack
        // shows the plural form (LIVE: 50 x iron fence = "Iron fencing"; LEAD: the tag's count
        // rule)
        const std::string nm = count > 0 ? cat->PocketItemName(l, it.id, it.free, count > 1) : "";
        names[i] = count > 0 ? GameCase(s, "LayoutMsg/MainScreen", "0101", nm) : "";
        o_bag.T(p + "name", names[i]);
        o_bag.T(p + "num", count >= 2
                               ? std::string{Key} + "num/outline/" + std::to_string(count) + "?h=25"
                               : "");
    }
    o_bag.I("bag.slots", slots);
    o_bag.I("bag.used", used);
    // r9 (owner 2026-10-03, reverses "always a selection"): nothing is selected until an item is
    // tapped; a tap on anything that is not an item (an empty slot, the page around the bag)
    // clears it, and an item that leaves its slot takes the selection with it
    if (bag_sel >= slots || bag_sel < 0 || s.pockets[bag_sel].Empty())
        bag_sel = -1;
    o_bag.I("bag.sel", bag_sel);
    // the name balloon is sized to the name (bag lane): runtime width at text_scale 10
    o_bag.I("bag.sel.name.w", bag_sel >= 0 ? widths.Measure(names[bag_sel]) : 0);
    if (s.miles >= 0)
        o_bag.T("miles.txt", Num(s.miles));
    if (s.bank >= 0)
        o_bag.T("bank.txt", Num(s.bank));
}

// ---- critters page ---------------------------------------------------------------------------
void Publisher::BuildCritters(const live::LiveSnapshot& s) {
    Catalog* cat = services.catalog;
    // the companion's Critterpedia keeps its own tab / filter / selection (owner 2026-10-02: it
    // does not follow the game's Critterpedia cursor)
    const int k = std::clamp(crit_kind, 0, 2);
    uint64_t key = Mix(0xc417, static_cast<uint64_t>(lang + 1));
    key = Mix(key, static_cast<uint64_t>(k * 16 + crit_filter));
    for (const auto& v : s.caught)
        key = MixBytes(key, v.data(), v.size() * 2);
    key = Mix(key, xs.collect_ok);
    key = MixBytes(key, xs.collect_bits.data(), xs.collect_bits.size());
    key = MixBytes(key, s.donated.data(), s.donated.size() * 2);
    key = Mix(key, static_cast<uint64_t>(crit_sel + 1));
    key = Mix(key, (uint64_t(s.mo) << 8) | uint64_t(s.hemi + 1));
    key = Mix(key, s.order_ok);
    key = Mix(key, s.case_table != nullptr);
    key = Mix(key, static_cast<uint64_t>(s.h));
    if (o_crit.key == key)
        return;
    o_crit.Clear();
    o_crit.key = key;
    if (!cat || !s.personal_ok)
        return;
    const Lang l = static_cast<Lang>(std::max(lang, 0));
    const auto& all = cat->Critters(k);
    // caught = the game's Critterpedia rule: the item's ItemCollectBit (xs.Collected; LIVE round 7,
    // r7-pages.md §5); the per-kind catch lists (Fish/Insect/DiveFishCollection) only as a fallback
    // while the bits are not read
    std::unordered_set<uint16_t> caught;
    if (xs.collect_ok) {
        for (const auto& c : all)
            if (xs.Collected(c.item))
                caught.insert(c.item);
    } else {
        caught.insert(s.caught[k].begin(), s.caught[k].end());
    }
    std::unordered_set<uint16_t> donated(s.donated.begin(), s.donated.end());
    // the Critterpedia's own list and order (PictureBook BYML, see the header); fallback when the
    // book code is not the pinned one or the file is unreadable: rows with a book illustration in
    // table order (crit.src 0)
    if (!book_tried && services.romfs) {
        book_tried = true;
        std::vector<uint8_t> pack, byml;
        static const char* const Files[] = {"ItemInsectBookData", "ItemFishBookData",
                                            "ItemSeafoodBookData"};
        if (services.romfs->Read("Pack/StaticParam.pack", pack))
            for (int i = 0; i < 3; ++i)
                if (!Romfs::SarcFind(
                        pack, std::string{"Param/Item/PictureBook/"} + Files[i] + ".byml", byml) ||
                    !BookByml(byml, book_items[i]))
                    book_items[i].clear();
    }
    std::vector<int> book;
    bool from_game = s.order_ok && !book_items[k].empty();
    if (from_game) {
        std::unordered_map<uint16_t, int> row_of;
        for (size_t i = 0; i < all.size(); ++i)
            row_of.emplace(all[i].item, static_cast<int>(i));
        for (const uint16_t it : book_items[k]) {
            const auto f = row_of.find(it);
            if (f == row_of.end()) {
                from_game = false; // an item the catalog does not know: do not guess
                break;
            }
            book.push_back(f->second);
        }
    }
    if (!from_game) {
        auto& illustrated = book_art;
        if (illustrated[k].empty() && services.romfs)
            for (size_t i = 0; i < all.size(); ++i)
                if (!all[i].book_icon.empty() &&
                    services.romfs->Exists("Model/" + all[i].book_icon + ".Nin_NX_NVN.zs"))
                    illustrated[k].push_back(static_cast<int>(i));
        book = illustrated[k];
    }
    // "available this month": any time slot of the month has a non-zero chance in the island's
    // hemisphere row (AppearArea 0 = north / 1 = south: LEAD) of the appear tables
    std::unordered_set<uint16_t> month_ok;
    if (crit_filter == 1 && s.clock_ok && s.hemi >= 0) {
        static const char* const InsectMonth[] = {"January",   "Februay", "March",    "April",
                                                  "May",       "June",    "July",     "August",
                                                  "September", "October", "November", "December"};
        const auto scan = [&](const char* table, const std::vector<std::string>& cols) {
            const Bcsv* t = cat->Table(table);
            if (!t)
                return;
            const uint32_t area_u8 = Bcsv::Key("AppearArea", "u8");
            for (size_t r = 0; r < t->Rows(); ++r) {
                if (t->Has(area_u8) && static_cast<int>(t->U(r, area_u8)) != s.hemi)
                    continue;
                if (!t->Has(area_u8) && t->Has(0x64330cb0) &&
                    t->U(r, 0x64330cb0) != Crc32(s.hemi ? "South" : "North"))
                    continue;
                for (const auto& c : cols)
                    if (t->Has(Bcsv::Key(c, "u16")) && t->U(r, Bcsv::Key(c, "u16")) > 0) {
                        month_ok.insert(static_cast<uint16_t>(t->U(r, Bcsv::Key("ItemID", "u16"))));
                        break;
                    }
            }
        };
        if (k == 0) {
            std::vector<std::string> cols;
            for (const char* slot :
                 {"Morning", "Daytime", "Evening1", "Evening2", "Night", "Midnight"})
                cols.push_back(std::string{"Prob"} + InsectMonth[s.mo - 1] + slot);
            scan("InsectAppearParam", cols);
        } else {
            std::vector<std::string> cols;
            for (const char* slot : {"MorningAndEvening", "Daytime", "Night"})
                cols.push_back(std::string{"Prob"} + MonthShort[s.mo - 1] + slot);
            if (k == 1) {
                scan("FishAppearRiverParam", cols);
                scan("FishAppearSeaParam", cols);
            } else {
                scan("SeafoodAppearParam", cols);
            }
        }
    }
    crit_rows.clear();
    int n_caught = 0, n_donated = 0;
    for (const int i : book) {
        const auto& c = all[i];
        const bool got = caught.contains(c.item);
        n_caught += got;
        n_donated += donated.contains(c.item);
        if (crit_filter == 2 && got)
            continue;
        if (crit_filter == 1 && !month_ok.contains(c.item))
            continue;
        crit_rows.push_back(i);
    }
    // The game's Critterpedia lays its list out column-major, 5 rows high, scrolling sideways (LIVE
    // round 7, sea tab: book entries 0 / 1 / 3 / 19 / 34 / 35 at column i / 5, row i % 5). The
    // companion's grid is 8 wide and scrolls down, so the unfiltered list is published in blocks of
    // the game's 8 columns x 5 rows: display cell d (row-major) = book entry 40 (d / 40) +
    // 5 (d % 8) + (d % 40) / 8. Each block then looks exactly like one screen of the game's
    // Critterpedia (the sea tab's 40 entries are one block). Filtered lists stay in book order.
    if (crit_filter == 0 && !crit_rows.empty() && crit_rows.size() % 40 == 0) {
        std::vector<int> grid(crit_rows.size());
        for (size_t d = 0; d < grid.size(); ++d)
            grid[d] = crit_rows[(d / 40) * 40 + (d % 8) * 5 + (d % 40) / 8];
        crit_rows = std::move(grid);
    }
    o_crit.I("crit.kind", k);
    o_crit.I("crit.filter", crit_filter);
    o_crit.I("crit.total", static_cast<int64_t>(book.size()));
    o_crit.I("crit.caught", n_caught);
    o_crit.I("crit.donated", n_donated);
    o_crit.I("crit.n", static_cast<int64_t>(crit_rows.size()));
    o_crit.I("crit.list", crit_list);
    char lab[8];
    std::snprintf(lab, sizeof lab, "%04d", 1001 + k);
    o_crit.I("crit.cat.w", widths.Measure(Msg("String/STR_CategoryName", lab)));
    for (size_t j = 0; j < crit_rows.size(); ++j) {
        const auto& c = all[crit_rows[j]];
        const std::string p = "crit." + std::to_string(j) + ".";
        const int state = donated.contains(c.item) && caught.contains(c.item) ? 2
                          : caught.contains(c.item)                           ? 1
                                                                              : 0;
        o_crit.I(p + "state", state);
        o_crit.T(p + "icon", std::string{Key} + "icon/crit/" + std::to_string(k) + "/" +
                                 std::to_string(c.uid) + "?s=101");
    }
    // the Critterpedia always has a cursor: entry 0 by default (-1 only for an empty list)
    if (crit_sel < 0 || crit_sel >= static_cast<int>(crit_rows.size()))
        crit_sel = crit_rows.empty() ? -1 : 0;
    o_crit.I("crit.sel", crit_sel);
    if (crit_sel != crit_par_sel || crit_list != crit_par_list) {
        crit_par ^= 1;
        crit_par_sel = crit_sel;
        crit_par_list = crit_list;
    }
    o_crit.I("crit.sel.par", crit_par);
    // "Current Active Hours" of the selected entry, as its detail page draws them: this month's
    // appear-table slots (island hemisphere row) as hour pieces of a 0-24 bar. Slot -> hours = the
    // detail layout's own bar panes (romfs Layout/LBookDetail W_InsectBar_* / W_FishBar_* and their
    // TimeOnOff groups, code 0x2e3bed0); LIVE-checked on the detail page (Oct, north): common
    // butterfly 4-19, moth 19-4, cricket 17-8, banded dragonfly 8-17, pale chub 9-16, dace and
    // catfish 16-9.
    {
        uint32_t hours = 0;
        if (crit_sel >= 0 && s.clock_ok && s.hemi >= 0)
            hours = ActiveHours(*cat, k, all[crit_rows[crit_sel]].item, s.mo, s.hemi);
        for (int h = 0; h < 24; ++h)
            o_crit.I("crit.sel.hr." + std::to_string(h), (hours >> h) & 1);
        o_crit.I("crit.now.h", s.clock_ok ? s.h : -1);
    }
    if (crit_sel < 0) {
        o_crit.T("crit.sel.name", "");
        o_crit.I("crit.sel.name.w", 0);
    } else {
        const auto& c = all[crit_rows[crit_sel]];
        // the caption plate: LEAD = the name window part's rule (Parts/PARTS_NameWindow 001
        // "{50:3}{item}"); live English shows "Common butterfly", "Flea"
        const std::string name =
            caught.contains(c.item)
                ? GameCase(s, "LayoutMsg/Parts/PARTS_NameWindow", "001", cat->ItemName(l, c.item))
                : "???";
        o_crit.T("crit.sel.name", name);
        o_crit.I("crit.sel.name.w", widths.Measure(name));
    }
}

// ---- DIY page --------------------------------------------------------------------------------
void Publisher::BuildDiy(const live::LiveSnapshot& s) {
    Catalog* cat = services.catalog;
    uint64_t key = Mix(0xd1, static_cast<uint64_t>(lang + 1));
    key = MixBytes(key, s.recipe_collect.data(), 0x100);
    key = MixBytes(key, s.recipe_made.data(), 0x100);
    key = MixBytes(key, s.recipe_new.data(), 0x100);
    key = MixBytes(key, s.recipe_fav.data(), 0x100);
    key = MixItems(key, s.pockets);
    key = Mix(key, static_cast<uint64_t>(s.chest_hash));
    key = Mix(key, (uint64_t(diy_cat + 1) << 32) | (uint64_t(diy_filter + 1) << 16) |
                       uint64_t(diy_sel + 1));
    for (const char* f :
         {"MainmenuRecipe_v2", "OpenDIYRecipeEtcCategory", "OpenDIYRecipeSeasonCategory",
          "HotelUnlockDIYTrade", "HasLookMyHouseExplain"})
        key = Mix(key, static_cast<uint64_t>(PlayerFlag(s, f) + 2));
    key = Mix(key, (uint64_t(s.y) << 24) | (uint64_t(s.mo) << 16) | (uint64_t(s.d) << 8) |
                       uint64_t(s.hemi + 1));
    key = Mix(key, s.case_table != nullptr);
    if (o_diy.key == key)
        return;
    o_diy.Clear();
    o_diy.key = key;
    if (!cat || !s.personal_ok)
        return;
    const Lang l = static_cast<Lang>(std::max(lang, 0));
    const auto bit = [](const std::array<uint8_t, 0x100>& a, unsigned u) {
        return u < 2048 && (a[u >> 3] >> (u & 7)) & 1;
    };
    std::unordered_map<uint16_t, int> have_p, have_s;
    for (int i = 0; i < s.pocket_slots && i < 40; ++i)
        if (!s.pockets[i].Empty())
            have_p[s.pockets[i].id] += StackCount(*cat, s.pockets[i]);
    for (const auto& it : s.chest)
        if (!it.Empty())
            have_s[it.id] += StackCount(*cat, it);
    const auto& recipes = cat->Recipes();
    // known recipes, by category
    std::vector<int> known;
    for (size_t i = 0; i < recipes.size(); ++i)
        if (bit(s.recipe_collect, recipes[i].uid))
            known.push_back(static_cast<int>(i));
    std::sort(known.begin(), known.end(), [&](int a, int b) {
        return std::pair{recipes[a].serial, recipes[a].uid} <
               std::pair{recipes[b].serial, recipes[b].uid};
    });
    const auto cat_of = [&](const Catalog::Recipe& r) {
        const auto* it = cat->FindItem(r.item);
        for (size_t c = 1; c < DiyCatUi.size(); ++c)
            for (const auto& u : DiyCatUi[c])
                if (it && it->ui == Crc32(u))
                    return static_cast<int>(c);
        return -1;
    };
    // The game's "can craft" (r9 polish, code + LIVE): every DIY list entry carries a craftable
    // byte (+0x25) set by the entry builder 0x251fb40 -> 0x2529c40: per material, the count in
    // the two pocket holders (+ the storage, only when the app's storage flag is set) must reach
    // the amount, else +0x25 = 0. The Craftable categories (both table types 2 and 25) and the
    // hammer badge read that byte; LIVE all 593 entries on s5c = pockets + storage with our stack
    // counts. Storage flag 0x32ffc10 (DIY app ctor 0x2e9aa64): player flag HasLookMyHouseExplain
    // != 0, and three conditions the publisher cannot see (scene attribute bits 0x11 / 0x19 clear,
    // a stage hash not 0xef1f0458 / 0xf70487f3, 0x2569430 = the player's house state: LEAD, they
    // hold on the own island). Material kinds the game counts by a special counter (0x19f3520 ->
    // 0x257f5b0, e.g. Money / Turnip rows: LEAD) are counted here as stacks.
    const bool storage = PlayerFlag(s, "HasLookMyHouseExplain") > 0;
    const auto can = [&](const Catalog::Recipe& r, bool with_storage) {
        for (const auto& [m, n] : r.materials) {
            const int h = (have_p.contains(m) ? have_p[m] : 0) +
                          (with_storage && storage && have_s.contains(m) ? have_s[m] : 0);
            if (h < n)
                return false;
        }
        return true;
    };
    // the app's category list and membership (see DiyCats); the v2 app (DIY Recipes+, player flag
    // MainmenuRecipe_v2) swaps Craftable for Craftable + storage and adds the cooking categories
    const bool v2 = PlayerFlag(s, "MainmenuRecipe_v2") > 0;
    const uint32_t diy_type = Crc32("DIY"), cook_type = Crc32("Cooking");
    const auto in_cat = [&](const DiyCatDef& c, const Catalog::Recipe& r) {
        switch (c.rule) {
        case DiyRule::All:
            return true;
        case DiyRule::Seasonal:
            return SeasonalRecipe(s, r);
        case DiyRule::Ui:
        case DiyRule::Other:
            return r.data_type == diy_type && cat_of(r) == c.icon;
        case DiyRule::Cooking:
            return r.data_type == cook_type && cat_of(r) == c.icon;
        case DiyRule::Craftable: // the entry's craftable byte (both types read +0x25)
            return can(r, true);
        case DiyRule::CraftableBoth:
            return can(r, true);
        case DiyRule::Favorites:
            return bit(s.recipe_fav, r.uid);
        case DiyRule::Requests:    // 0x2e9fccc + ImmQuest checks: not decoded (LEAD), never listed
        case DiyRule::DiyRequests: // the hotel trade requests: not decoded (LEAD)
            return false;
        }
        return false;
    };
    const auto count = [&](const DiyCatDef& c) {
        return std::count_if(known.begin(), known.end(),
                             [&](int i) { return in_cat(c, recipes[i]); });
    };
    diy_cats.clear();
    for (size_t c = 0; c < std::size(DiyCats); ++c) {
        const auto& d = DiyCats[c];
        bool on = false;
        switch (d.rule) {
        case DiyRule::All:
            on = true;
            break;
        case DiyRule::Favorites:
        case DiyRule::Craftable:
            on = false;
            break;
        case DiyRule::CraftableBoth:
            on = false;
            break;
        case DiyRule::Ui:
            on = count(d) > 0;
            break;
        case DiyRule::Other: // the flag is set by the app itself once the category has a recipe
            on = PlayerFlag(s, "OpenDIYRecipeEtcCategory") > 0 || count(d) > 0;
            break;
        case DiyRule::Seasonal:
            on = PlayerFlag(s, "OpenDIYRecipeSeasonCategory") > 0;
            break;
        case DiyRule::Cooking:
            on = v2 && count(d) > 0;
            break;
        case DiyRule::DiyRequests:
            on = PlayerFlag(s, "HotelUnlockDIYTrade") > 0;
            break;
        case DiyRule::Requests:
            on = false; // LEAD (not decoded)
            break;
        }
        if (on)
            diy_cats.push_back(static_cast<int>(c));
    }
    if (diy_cat < 0 || diy_cat >= static_cast<int>(diy_cats.size()))
        diy_cat = 0;
    const DiyCatDef& cdef = DiyCats[diy_cats[diy_cat]];
    diy_rows.clear();
    for (const int i : known) {
        const auto& r = recipes[i];
        if (!in_cat(cdef, r))
            continue;
        // "Craftable" chip = the game's craftable set (the hammer badge / Craftable category rule),
        // not pockets only (owner r9: the chip listed fewer recipes than had enough materials)
        if (diy_filter == 1 && !can(r, true))
            continue;
        if (diy_filter == 2 && !bit(s.recipe_fav, r.uid))
            continue;
        if (diy_filter == 3 && bit(s.recipe_made, r.uid))
            continue;
        diy_rows.push_back(i);
    }
    o_diy.I("diy.known", static_cast<int64_t>(known.size()));
    o_diy.I("diy.total", static_cast<int64_t>(recipes.size()));
    o_diy.I("diy.n", static_cast<int64_t>(diy_rows.size()));
    o_diy.I("diy.list", diy_list);
    o_diy.I("diy.cat", diy_cat);
    o_diy.I("diy.cat.n", static_cast<int64_t>(diy_cats.size()));
    for (size_t c = 0; c < diy_cats.size(); ++c)
        o_diy.I("diy.cat." + std::to_string(c) + ".id", DiyCats[diy_cats[c]].icon);
    // an empty list shows the app's own message (MenuRecipe 0901 favourites, 0902 craftable,
    // 0905 seasonal, 0904 any other category)
    const char* empty = cdef.rule == DiyRule::Favorites ? "0901"
                        : cdef.rule == DiyRule::Craftable || cdef.rule == DiyRule::CraftableBoth
                            ? "0902"
                        : cdef.rule == DiyRule::Seasonal ? "0905"
                                                         : "0904";
    o_diy.T("diy.empty.txt", diy_rows.empty() ? Msg("LayoutMsg/MenuRecipe", empty) : "");
    const std::string cname = Msg("String/STR_CategoryName", cdef.label);
    o_diy.T("diy.cat.sel.name", cname);
    o_diy.I("diy.cat.sel.w", widths.Measure(cname));
    o_diy.I("diy.filter", diy_filter);
    // the DIY app always has a cursor: recipe 0 by default (-1 only for an empty list)
    if (diy_sel < 0 || diy_sel >= static_cast<int>(diy_rows.size()))
        diy_sel = diy_rows.empty() ? -1 : 0;
    o_diy.I("diy.sel", diy_sel);
    for (size_t j = 0; j < diy_rows.size(); ++j) {
        const auto& r = recipes[diy_rows[j]];
        std::string card = std::string{Key} + "ui/diy/card/" + std::to_string(r.uid) + "?w=114";
        if (bit(s.recipe_fav, r.uid))
            card += "&fav=1";
        if (bit(s.recipe_made, r.uid))
            card += "&made=1";
        // the card's badge is the hammer (LRecipeBtn N_CanMake_00 = IconCatCanMake_64) whenever the
        // materials are in the pockets + storage (LIVE: flimsy axe with every branch in storage)
        if (can(r, true))
            card += "&can=1";
        // the made item's whereabouts (LIVE: storage mark on the balancing toy; pockets first:
        // LEAD)
        if (have_p.contains(r.item))
            card += "&pocket=1";
        else if (have_s.contains(r.item))
            card += "&stor=1";
        if (bit(s.recipe_new, r.uid))
            card += "&new=1";
        if (static_cast<int>(j) == diy_sel)
            card += "&sel=1";
        o_diy.T("diy." + std::to_string(j) + ".card", card);
    }
    if (diy_sel < 0) {
        o_diy.I("diy.sel.fav", 0);
        o_diy.I("diy.sel.favarg", -1);
        o_diy.I("diy.sel.n", 0);
        o_diy.I("diy.sel.cat", -1);
        o_diy.I("diy.sel.craft", 0);
        o_diy.T("diy.sel.icon", "");
        o_diy.T("diy.sel.name", "");
        o_diy.T("diy.sel.size", "");
        o_diy.I("diy.sel.size.ok", 0);
        o_diy.T("diy.sel.size.icon", "");
        for (int j = 0; j < 6; ++j) {
            const std::string p = "diy.sel.m" + std::to_string(j) + ".";
            o_diy.T(p + "icon", "");
            o_diy.T(p + "name", "");
            o_diy.I(p + "have", 0);
            o_diy.I(p + "need", 0);
            o_diy.I(p + "stor", 0);
        }
    } else {
        const auto& r = recipes[diy_rows[diy_sel]];
        const int n = static_cast<int>(r.materials.size());
        // the detail pane's star (r9): the recipe's RecipeFavoriteBit; a tap sends diy_fav with
        // uid * 2 + the wanted state (the bag writer re-reads the bit and writes one byte)
        const bool fav = bit(s.recipe_fav, r.uid);
        o_diy.I("diy.sel.fav", fav ? 1 : 0);
        o_diy.I("diy.sel.favarg", static_cast<int64_t>(r.uid) * 2 + (fav ? 0 : 1));
        const auto* item = cat->FindItem(r.item);
        const auto units = [](int v) { return std::to_string(v / 2) + (v % 2 ? ".5" : ""); };
        const bool sized = item && item->width2 > 0 && item->height2 > 0;
        o_diy.I("diy.sel.size.ok", sized);
        o_diy.T("diy.sel.size.icon", sized ? std::string{Key} + "ui/diy/sizegrid?s=44&x=" +
                                                 std::to_string(item->width2) +
                                                 "&z=" + std::to_string(item->height2)
                                           : "");
        o_diy.T("diy.sel.size", sized ? units(item->width2) + " × " + units(item->height2) : "");
        o_diy.I("diy.sel.n", n);
        o_diy.I("diy.sel.cat", std::max(0, cat_of(r)));
        o_diy.I("diy.sel.craft", can(r, false) ? 1 : can(r, true) ? 2 : 0);
        o_diy.T("diy.sel.icon", std::string{Key} + "icon/itemart/" + std::to_string(r.item) +
                                    (n <= 3 ? "?s=190" : "?s=120"));
        o_diy.T("diy.sel.name",
                GameCase(s, "LayoutMsg/MenuRecipe", "0201", cat->ItemName(l, r.item)));
        for (int j = n; j < 6; ++j) {
            const std::string p = "diy.sel.m" + std::to_string(j) + ".";
            o_diy.T(p + "icon", "");
            o_diy.T(p + "name", "");
            o_diy.I(p + "have", 0);
            o_diy.I(p + "need", 0);
            o_diy.I(p + "stor", 0);
        }
        for (int j = 0; j < n && j < 6; ++j) {
            const auto [m, need] = r.materials[j];
            const std::string p = "diy.sel.m" + std::to_string(j) + ".";
            o_diy.T(p + "icon", std::string{Key} + "icon/item/" + std::to_string(m) + "?s=56");
            o_diy.T(p + "name", GameCase(s, "LayoutMsg/MenuRecipe", "1201", cat->ItemName(l, m)));
            o_diy.I(p + "have", have_p.contains(m) ? have_p[m] : 0);
            o_diy.I(p + "need", need);
            o_diy.I(p + "stor", storage && have_s.contains(m) ? have_s[m] : 0);
        }
        // "Pockets: n" / "Storage: n" (MenuRecipe 1702 / 1704 + 1703): the count of the made item
        const std::string sep = Msg("LayoutMsg/MenuRecipe", "1703", ": ");
        const std::string pk = Msg("LayoutMsg/MenuRecipe", "1702", "Pockets") + sep +
                               std::to_string(have_p.contains(r.item) ? have_p[r.item] : 0);
        const std::string st = Msg("LayoutMsg/MenuRecipe", "1704", "Storage") + sep +
                               std::to_string(have_s.contains(r.item) ? have_s[r.item] : 0);
        o_diy.T("diy.sel.pockets.txt", pk);
        o_diy.T("diy.sel.storage.txt", st);
        o_diy.I("diy.sel.pockets.txt.w", widths.Measure(pk));
        o_diy.I("diy.sel.storage.txt.w", widths.Measure(st));
    }
}

} // namespace acnh
