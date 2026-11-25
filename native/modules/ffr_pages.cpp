// SPDX-FileCopyrightText: Copyright 2026 Daniel Vergara
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ffr_pages.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ffr {
namespace {

// Palette sampled from the game's menu art (T_BtlWindow_*, T_CharaListBG*, T_CommonListSelect, list labels).
constexpr Rgba kInk = C(0xFFF2F5F7), kInkDim = C(0xFFA7B8C4), kInkFaint = C(0xFF6E8392);
constexpr Rgba kNavy = C(0xFF0A131B), kPanel = C(0xD0213E4F);
constexpr Rgba kLine = C(0xFF5EC4E6), kLineDim = C(0x805EC4E6), kGold = C(0xFFE6C36B);
constexpr Rgba kHp = C(0xFF7ED957), kHpBack = C(0xFF22362A), kMp = C(0xFF58B7F5), kMpBack = C(0xFF1C2D3C);
constexpr Rgba kShadow = C(0xA0000000);
constexpr Rgba kFoe = C(0xFFE8704F), kFoeBack = C(0xFF3A2220), kFoeInk = C(0xFFF5B4A4);  // enemy side

const char* kUi = "FFRS/Content/UI/Textures/";
std::string Ui(const std::string& rel) { return std::string(kUi) + rel + ".uasset"; }

std::string Num(long long v) {
    std::string s = std::to_string(v < 0 ? -v : v), o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0) o += ',';
        o += s[i];
    }
    return v < 0 ? "-" + o : o;
}

std::string ItemName(const Art& a, int id) {
    std::string s;
    if (id < 10000) s = a.text("ST_ConsumablesName", "ConsumablesName" + std::to_string(id));
    else if (id < 13000) s = a.text("ST_EquipmentName", "EquipmentName" + std::to_string(id));
    else if (id < 14000) s = a.text("ST_VisionName", "VisionName" + std::to_string(id));
    else if (id < 30000) s = a.text("ST_ValuableName", "ValuableName" + std::to_string(id));
    else s = a.text("ST_ArtifactName", "ArtifactName" + std::to_string(id));
    return s.empty() ? "#" + std::to_string(id) : s;
}
std::string ItemDesc(const Art& a, int id) {
    std::string s;
    if (id < 10000) s = a.text("ST_ConsumablesDescription", "ConsumablesDescription" + std::to_string(id));
    else if (id < 13000) s = a.text("ST_EquipmentDescription", "EquipmentDescription" + std::to_string(id));
    else if (id < 14000) s = a.text("ST_VisionDescription", "VisionDescription" + std::to_string(id));
    else if (id < 30000) s = a.text("ST_ValuableDescription", "ValuableDescription" + std::to_string(id));
    else s = a.text("ST_ArtifactDescription", "ArtifactDescription" + std::to_string(id));
    return s == "None" ? "" : s;
}
int Category(int id) {
    if (id < 10000) return 0;
    if (id < 14000) return 1;
    if (id < 30000) return 2;
    return 3;
}
const GameState* g_icon_state = nullptr;  // the state ComposePage draws (equipment detail types)
std::string ItemIcon(int id) {
    if (g_icon_state && id >= 10000 && id < 13000) {
        auto it = g_icon_state->equipment.find(id);
        if (it != g_icon_state->equipment.end()) {
            static const char* kDetail[12] = {nullptr, "ICON_Item_Brade", "ICON_Item_Katana", "ICON_Item_Bow", "ICON_Item_Hummer", "ICON_Item_Book",
                                              "ICON_Item_Gun", "ICON_Item_Rod", "ICON_Item_Whip", "ICON_Item_HeavyArmor", "ICON_Item_LightArmor", "ICON_Item_Dress"};
            const int d = it->second.detail;
            if (d > 0 && d < 12) return Ui(std::string("Icon/Menu/") + kDetail[d]);
            if (it->second.type == 3) return Ui("Icon/Menu/ICON_Item_Ling");
        }
    }
    if (id < 10000) return Ui("Icon/Menu/ICON_Battle_Item");
    if (id < 11000) return Ui("Icon/Menu/ICON_Item_Brade");
    if (id < 12000) return Ui("Icon/Menu/ICON_Item_LightArmor");
    if (id < 13000) return Ui("Icon/Menu/ICON_Item_Ling");
    if (id < 14000) return Ui("Icon/Menu/ICON_Item_Crystal");
    if (id < 30000) return Ui("Icon/Menu/ICON_Item_Key");
    return Ui("Icon/Menu/ICON_Item_Kira");
}
// A vision's face (Icon/Vision_Face, named by the vision's id), as the game's vision menus show it
const Image* VisionFace(const Art& a, int id) { return id >= 13000 && id < 14000 ? a.image(Ui("Icon/Vision_Face/ICON_UnitFace_summon" + std::to_string(id))) : nullptr; }
// The vision as a round face in a gold ring (falls back to the crystal icon)
void VisionBadge(Canvas& c, const Art& a, int id, int x, int y, int d) {
    const float r = d * .5f;
    c.Disc(x + r, y + r, r, kGold);
    c.Disc(x + r, y + r, r - 2, C(0xFF101E28));
    if (const Image* f = VisionFace(a, id)) c.BlitFit(*f, x + 3, y + 3, d - 6, d - 6);
    else if (const Image* ic = a.image(Ui("Icon/Menu/ICON_Item_Crystal"))) c.Blit(*ic, x + 3, y + 3, d - 6, d - 6);
}
std::string UnitName(const Art& a, int id) {
    std::string s = a.text("ST_UnitName", "UnitName" + std::to_string(id));
    return s.empty() ? "Unit " + std::to_string(id) : s;
}

void Background(Canvas& c, const Art& a) {
    // The same picture behind every page: drawn once (the menu art scaled to the page is most of a page's cost), then copied
    static std::vector<uint8_t> cached;
    if (cached.size() == c.pixels().size()) { c.CopyFrom(cached); return; }
    c.Clear(kNavy);
    const Image* bg = a.image(std::string("FFRS/Content/UI/MainMenu/Common/BG/Textures/T_MenuBG001.uasset"));
    if (bg) c.BlitFit(*bg, 0, 0, kW, kH, 0.55f, true);
    c.GradientV(0, 0, kW, kH / 2, C(0x700A131B), C(0xC00A131B));
    c.GradientV(0, kH / 2, kW, kH / 2, C(0xC00A131B), C(0xF00A131B));
    if (bg) cached = c.pixels();
}

// Words of s that fit maxw at px, line by line (explicit newlines kept)
std::vector<std::string> Wrap(const Font& f, const std::string& s, float px, float maxw) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        const std::string para = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::string line, word;
        auto flush_word = [&] {
            if (word.empty()) return;
            const std::string tryl = line.empty() ? word : line + " " + word;
            if (!line.empty() && f.Measure(tryl, px) > maxw) out.push_back(line), line = word;
            else line = tryl;
            word.clear();
        };
        for (char ch : para) {
            if (ch == ' ') flush_word();
            else if (ch != '\r') word += ch;
        }
        flush_word();
        out.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

void GlowLine(Canvas& c, int x, int y, int w, Rgba col) {
    c.GradientH(x, y, w / 2, 2, C(0x005EC4E6), col);
    c.GradientH(x + w / 2, y, w - w / 2, 2, col, C(0x005EC4E6));
}

void Panel(Canvas& c, const Art& a, int x, int y, int w, int h, bool lit = false) {
    c.Fill(x, y, w, h, lit ? C(0xE02A5568) : kPanel);
    c.GradientV(x, y, w, h / 3, C(0x30FFFFFF), C(0x00FFFFFF));
    c.Frame(x, y, w, h, 2, lit ? kLine : kLineDim);
    (void)a;
}

void Header(Canvas& c, const GameState& s, const Art& a, const std::string& title) {
    c.GradientV(0, 0, kW, kHeaderH, C(0xF0101E28), C(0xA0101E28));
    if (const Image* star = a.image(Ui("Menu/T_HeaderStarIcon"))) c.Blit(*star, 34, 34, 44, 44);
    c.Text(*a.title, title, 92, 74, 54, kInk, Align::Left, 640, kShadow);
    GlowLine(c, 0, kHeaderH - 2, kW, kLine);
    if (s.save_ok) {
        const std::string gil = Num(s.money);
        const float gw = a.bold->Measure(gil, 38);
        if (const Image* gi = a.image(Ui("Icon/Menu/ICON_Menu_GILL"))) c.Blit(*gi, int(950 - gw - 54), 34, 44, 44);
        c.Text(*a.bold, gil, 950, 72, 38, kInk, Align::Right, 0, kShadow);
        c.Text(*a.regular, "Gil", 958, 72, 24, kGold);
        const int t = int(s.play_time);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%d:%02d", t / 3600, (t / 60) % 60);
        c.Fill(1010, 34, 2, 44, kLineDim);
        c.Text(*a.regular, "Time", 1030, 72, 24, kInkDim);
        c.Text(*a.bold, buf, 1208, 72, 38, kInk, Align::Right, 0, kShadow);
    }
}

void TabBar(Canvas& c, const Art& a, int tab, bool map_ok) {
    c.GradientV(0, kTabBarY, kW, kH - kTabBarY, C(0xC0101E28), C(0xF8081118));
    GlowLine(c, 0, kTabBarY, kW, kLineDim);
    const char* names[kTabCount] = {"Map", "Party", "Items", "Quests"};
    const std::string icons[kTabCount] = {Ui("Icon/MAP/ICON_MAP_L_Town"), Ui("Icon/Menu/ICON_Battle_Fight"), Ui("Icon/Menu/ICON_Battle_Item"),
                                         Ui("Icon/MAP/ICON_MAP_L_Check")};
    const int tw = kW / kTabCount;
    for (int i = 0; i < kTabCount; ++i) {
        const int x = i * tw;
        if (i == tab) {
            if (const Image* sel = a.image(Ui("Menu/T_CommonListSelect"))) c.Blit(*sel, x + 8, kTabBarY + 10, tw - 16, kH - kTabBarY - 20, 0.9f);
            else c.Fill(x + 8, kTabBarY + 10, tw - 16, kH - kTabBarY - 20, C(0x605EC4E6));
            GlowLine(c, x + 20, kH - 14, tw - 40, kLine);
        }
        const bool off = i == kTabMap && !map_ok;  // no map picture for this area: shown dimmed, its tap is off (the manifest's ffr.map_tab)
        if (const Image* ic = a.image(icons[i])) c.Blit(*ic, x + 34, kTabBarY + 26, 60, 60, i == tab ? 1.f : off ? 0.22f : 0.6f);
        c.Text(*a.bold, names[i], x + 108, kTabBarY + 70, 34, i == tab ? kInk : off ? kInkFaint : kInkDim, Align::Left, tw - 116, kShadow);
    }
}

void Bar(Canvas& c, int x, int y, int w, int h, int v, int mx, Rgba fill, Rgba back) {
    c.Fill(x, y, w, h, back);
    if (mx > 0 && v > 0) {
        const int fw = int(std::lround(w * std::min(1.0, double(v) / mx)));
        c.GradientV(x, y, fw, h, fill, Rgba{uint8_t(fill.r * 3 / 4), uint8_t(fill.g * 3 / 4), uint8_t(fill.b * 3 / 4), 255});
    }
    c.Frame(x, y, w, h, 1, C(0x60FFFFFF));
}

void Stat(Canvas& c, const Art& a, int x, int y, const char* label, int hp, int mx, Rgba fill, Rgba back, int w) {
    c.Text(*a.bold, label, x, y + 4, 26, kInkDim);
    std::string val = Num(hp);
    if (mx > 0) val += " / " + Num(mx);
    c.Text(*a.bold, val, x + w, y + 4, 30, kInk, Align::Right, 0, kShadow);
    Bar(c, x, y + 14, w, 10, hp, mx > 0 ? mx : hp, fill, back);
}

// ---- Party ----
void PartyList(Canvas& c, const GameState& s, const Art& a) {
    auto roster = Roster(s);
    int y = kPartyCardY;
    for (size_t i = 0; i < roster.size() && i < 4; ++i, y += kPartyCardH + kPartyCardGap) {
        const UnitState& u = *roster[i];
        const bool in_party = std::find(s.party.begin(), s.party.end(), u.id) != s.party.end();
        Panel(c, a, 30, y, kW - 60, kPartyCardH, in_party);
        if (const Image* art = a.image(Ui("Picture/Char/T_Chara_Button_" + std::to_string(u.id)))) {
            c.SetFadeH(330, 600);
            c.BlitFit(*art, 32, y + 2, 600, kPartyCardH - 4, 1.f, true);
            c.SetFadeH(0, 0);
        }
        if (in_party) {
            const int n = int(std::find(s.party.begin(), s.party.end(), u.id) - s.party.begin()) + 1;
            if (const Image* num = a.image(Ui("Icon/Menu/T_menu_party_num_0" + std::to_string(std::min(n, 4))))) c.Blit(*num, 44, y + 14, 52, 52);
        }
        c.Text(*a.bold, UnitName(a, u.id), 520, y + 62, 46, kInk, Align::Left, 300, kShadow);
        if (u.level > 0) {
            c.Text(*a.regular, "Lv", 860, y + 62, 26, kInkDim);
            c.Text(*a.bold, std::to_string(u.level), 940, y + 62, 40, kInk, Align::Right);
        }
        Stat(c, a, 520, y + 96, "HP", u.hp, u.max_hp, kHp, kHpBack, 330);
        Stat(c, a, 520, y + 146, "MP", u.mp, u.max_mp, kMp, kMpBack, 330);
        // equipment: weapon, armor, and the vision equipped to this member (a face in a gold ring, its name in gold)
        const int ex = 900;
        int ey = y + 70;
        auto eq = [&](int id) {
            if (id <= 0) return;
            if (const Image* ic = a.image(ItemIcon(id))) c.Blit(*ic, ex, ey, 34, 34);
            c.Text(*a.regular, ItemName(a, id), ex + 44, ey + 26, 22, kInkDim, Align::Left, 230);
            ey += 40;
        };
        eq(u.weapon);
        eq(u.armor);
        if (u.vision > 0) {
            VisionBadge(c, a, u.vision, ex - 2, ey - 2, 38);
            c.Text(*a.bold, ItemName(a, u.vision), ex + 44, ey + 26, 22, kGold, Align::Left, 230);
        }
    }
    if (roster.empty()) c.Text(*a.regular, "No party data yet.", kW / 2, 500, 36, kInkDim, Align::Center);
    else c.Text(*a.regular, "Tap a member for details", kW / 2, kTabBarY - 18, 24, kInkFaint, Align::Center);
}

void PartyDetail(Canvas& c, const GameState& s, const Art& a, const UnitState& u) {
    if (const Image* p = a.image(Ui("Picture/Char/Long/T_Chara_Menu_long_" + std::to_string(u.id)))) {
        c.BlitFit(*p, 0, kHeaderH - 40, 520, kTabBarY - kHeaderH + 40, 1.f);
        c.GradientH(330, kHeaderH, 190, kTabBarY - kHeaderH, C(0x000A131B), C(0x000A131B));
    }
    const int x = 500, w = kW - x - 30;
    Panel(c, a, x, 132, w, 240);
    c.Text(*a.bold, UnitName(a, u.id), x + 28, 196, 54, kInk, Align::Left, w - 200, kShadow);
    if (u.level > 0) {
        c.Text(*a.regular, "Lv", x + w - 150, 196, 28, kInkDim);
        c.Text(*a.bold, std::to_string(u.level), x + w - 24, 196, 48, kInk, Align::Right);
    }
    Stat(c, a, x + 28, 248, "HP", u.hp, u.max_hp, kHp, kHpBack, w - 56);
    Stat(c, a, x + 28, 308, "MP", u.mp, u.max_mp, kMp, kMpBack, w - 56);
    c.Text(*a.regular, "EXP", x + 28, 356, 24, kInkDim);
    c.Text(*a.bold, Num(u.xp), x + w - 28, 356, 28, kInk, Align::Right);

    // Stats (shown when the live parameters are known)
    static const char* kStatNames[6] = {"Attack", "Defense", "Magic", "Spirit", "Divine", "Agility"};
    static const char* kStatIcons[6] = {"ICON_Status_attack", "ICON_Status_defense", "ICON_Status_magic", "ICON_Status_mental", "ICON_Status_divine", "ICON_Status_speed"};
    int y = 392;
    if (u.stats[0] >= 0) {
        Panel(c, a, x, y, w, 196);
        for (int i = 0; i < 6; ++i) {
            const int cx = x + 28 + (i % 2) * (w / 2), cy = y + 26 + (i / 2) * 56;
            if (const Image* ic = a.image(Ui(std::string("Icon/Menu/") + kStatIcons[i]))) c.Blit(*ic, cx, cy, 40, 40);
            c.Text(*a.regular, kStatNames[i], cx + 52, cy + 30, 26, kInkDim);
            c.Text(*a.bold, std::to_string(u.stats[i]), cx + w / 2 - 60, cy + 30, 32, kInk, Align::Right);
        }
        y += 212;
    }
    Panel(c, a, x, y, w, kTabBarY - 24 - y);
    c.Text(*a.title, "Equipment", x + 28, y + 52, 36, kGold);
    GlowLine(c, x + 20, y + 66, w - 40, kLineDim);
    int ey = y + 84;
    const int nacc = int(std::max<size_t>(1, u.accessories.size()));
    const int rh = std::clamp((kTabBarY - 30 - ey) / (3 + nacc), 44, 60);  // every row fits, the vision's too
    auto row = [&](const char* label, int id, bool vision = false) {
        if (ey + rh > kTabBarY - 30) return;
        const int mid = ey + rh / 2;
        c.Text(*a.regular, label, x + 28, mid + 8, 22, vision ? kGold : kInkFaint);
        if (id > 0) {
            if (vision) VisionBadge(c, a, id, x + 168, mid - 24, 48);
            else if (const Image* ic = a.image(ItemIcon(id))) c.Blit(*ic, x + 170, mid - 22, 44, 44);
            c.Text(*a.bold, ItemName(a, id), x + 226, mid + 12, 30, vision ? kGold : kInk, Align::Left, w - 250);
        } else {
            c.Text(*a.regular, "-", x + 226, mid + 12, 30, kInkFaint);
        }
        ey += rh;
    };
    row("Vision", u.vision, true);
    row("Weapon", u.weapon);
    row("Armor", u.armor);
    for (int i = 0; i < nacc; ++i) row("Accessory", i < int(u.accessories.size()) ? u.accessories[size_t(i)] : -1);
    (void)s;
    c.Text(*a.regular, "Tap anywhere to go back", 260, kTabBarY - 18, 22, kInkFaint, Align::Center);
}

// ---- Items ----
const char* kCatNames[4] = {"Items", "Equipment", "Key Items", "Artifacts"};

void CategoryBar(Canvas& c, const View& v, const Art& a) {
    const std::string icons[4] = {Ui("Icon/Menu/ICON_Battle_Item"), Ui("Icon/Menu/ICON_Item_Brade"), Ui("Icon/Menu/ICON_Item_Key"), Ui("Icon/Menu/ICON_Item_Kira")};
    const int tw = (kW - 60) / 4;
    for (int i = 0; i < 4; ++i) {
        const int x = 30 + i * tw;
        Panel(c, a, x + 4, 128, tw - 8, 76, i == v.item_cat);
        if (const Image* ic = a.image(icons[i])) c.Blit(*ic, x + 20, 140, 52, 52, i == v.item_cat ? 1.f : .55f);
        c.Text(*a.bold, kCatNames[i], x + 82, 178, 28, i == v.item_cat ? kInk : kInkDim, Align::Left, tw - 96);
    }
}

void RowBack(Canvas& c, const Art& a, int x, int y, int w, int h, bool sel, int r) {
    if (sel) {
        if (const Image* hl = a.image(Ui("Menu/T_CommonListSelect"))) c.Blit(*hl, x, y, w, h);
        else c.Fill(x, y, w, h, C(0x605EC4E6));
    } else {
        c.Fill(x, y, w, h, r % 2 ? C(0x60142632) : C(0x80142632));
    }
}

void ScrollBar(Canvas& c, int x, int top, int rows, int row_h, int first, int count) {
    if (count <= rows) return;
    const int track = rows * row_h;
    const int th = std::max(40, track * rows / count);
    const int ty = top + (track - th) * first / std::max(1, count - rows);
    c.Fill(x, top, 6, track, C(0x40FFFFFF));
    c.Fill(x, ty, 6, th, kLine);
}

void Description(Canvas& c, const Art& a, int x, int y, int w, int h, const std::string& text, const std::string& hint) {
    Panel(c, a, x, y, w, h);
    c.SetClip(x, y, w, h);  // nothing spills onto what is below
    if (!text.empty()) {
        const auto lines = Wrap(*a.regular, text, 27, float(w - 44));
        const int max_lines = std::max(1, (h - 16) / 34);
        for (int i = 0; i < int(lines.size()) && i < max_lines; ++i) {
            const bool last = i == max_lines - 1 && int(lines.size()) > max_lines;
            c.Text(*a.regular, last ? lines[size_t(i)] + " ..." : lines[size_t(i)], x + 22, y + 38 + 34 * i, 27, kInk, Align::Left, float(w - 44));
        }
    } else {
        c.Text(*a.regular, hint, x + 22, y + h / 2 + 10, 26, kInkFaint, Align::Left, float(w - 44));
    }
    c.ResetClip();
}

const View::Fx* FxFor(const View& v, int unit, int slot) {
    for (auto& f : v.fx)
        if (f.unit == unit && f.slot == slot) return &f;
    return nullptr;
}

// Why the game does not take a change now (View::act), for the line above the cards or the choices
const char* ActWhy(int act, bool equip) {
    switch (act) {
    case View::kActBattle: return equip ? "Equipment can be changed after the battle" : "In a battle, use items from the battle page";
    case View::kActBusy: return equip ? "Wait for the scene to end to change equipment" : "Wait for the scene to end to use items";
    case View::kActNoHelper: return "The companion's game helper is not running";
    default: return "";
    }
}

// Key Items and Artifacts: the game's list, with descriptions
void ItemList(Canvas& c, const GameState& s, const View& v, const Art& a) {
    auto items = ItemsOf(s, v.item_cat);
    const int top = kItemListY;
    for (int r = 0; r < kItemRows; ++r) {
        const int idx = v.item_scroll + r;
        if (idx >= int(items.size())) break;
        const int y = top + r * kItemRowH;
        RowBack(c, a, 30, y, kW - 60, kItemRowH - 6, idx == v.item_sel, r);
        const int id = items[size_t(idx)].id;
        if (const Image* ic = a.image(ItemIcon(id))) c.Blit(*ic, 46, y + 8, 62, 62);
        const float qty_w = a.bold->Measure(std::to_string(items[size_t(idx)].num), 38) + a.regular->Measure("x", 28) + 10;
        c.Text(*a.bold, ItemName(a, id), 128, y + 52, 36, kInk, Align::Left, float(1190 - qty_w - 24 - 128), kShadow);
        c.Text(*a.regular, "x", 1190 - qty_w + 4, y + 52, 28, kInkDim);
        c.Text(*a.bold, std::to_string(items[size_t(idx)].num), 1190, y + 54, 38, kInk, Align::Right);
    }
    if (items.empty()) c.Text(*a.regular, "Nothing here yet.", kW / 2, 480, 34, kInkDim, Align::Center);
    ScrollBar(c, kW - 18, top, kItemRows, kItemRowH, v.item_scroll, int(items.size()));
    const int dy = top + kItemRows * kItemRowH + 6;
    Description(c, a, 30, dy, kW - 60, kTabBarY - dy - 14,
                v.item_sel >= 0 && v.item_sel < int(items.size()) ? ItemDesc(a, items[size_t(v.item_sel)].id) : "",
                "Tap an item to read its description. Swipe to scroll.");
}

ConsumableInfo InfoOf(const GameState& s, int id) {
    auto it = s.consumables.find(id);
    return it == s.consumables.end() ? ConsumableInfo{} : it->second;
}

// The tag a recovery item shows: what it restores, in the colour of that bar (gold: a revive), "Battle" when it is not for the map
void ItemTag(Canvas& c, const Art& a, const ConsumableInfo& ci, float right, int y) {
    std::string t;
    Rgba col = kInkFaint, back = C(0x40FFFFFF);
    if (!ci.field) t = "Battle";
    else if (ci.kind == ConsumableInfo::kHp) t = ci.party ? "Party HP" : "HP", col = kHp, back = C(0x5022362A);
    else if (ci.kind == ConsumableInfo::kMp) t = ci.party ? "Party MP" : "MP", col = kMp, back = C(0x501C2D3C);
    else if (ci.kind == ConsumableInfo::kRevive) t = "Revive", col = kGold, back = C(0x50302A18);
    else t = "Field";
    const float w = a.bold->Measure(t, 22) + 28;
    c.Fill(int(right - w), y - 22, int(w), 32, back);
    c.Frame(int(right - w), y - 22, int(w), 32, 1, col);
    c.Text(*a.bold, t, right - w / 2, y + 2, 22, col, Align::Center);
}

struct CardBox { int x, y, w, h; };
std::vector<CardBox> UseCards(size_t n) {
    std::vector<CardBox> out;
    if (!n) return out;
    const int gap = 14, w = std::min(420, int((kW - 60 - gap * int(n - 1)) / int(n)));
    const int total = w * int(n) + gap * int(n - 1), x0 = (kW - total) / 2;
    for (size_t i = 0; i < n; ++i) out.push_back({x0 + int(i) * (w + gap), kUseCardY, w, kUseCardH});
    return out;
}

// Items: the recovery items with what each restores; below, the party as cards to tap. An item, then a member: the game uses it.
void ItemsUse(Canvas& c, const GameState& s, const View& v, const Art& a) {
    auto items = ItemsOf(s, 0);
    const int top = kItemListY;
    const bool has_sel = v.item_sel >= 0 && v.item_sel < int(items.size());
    const ConsumableInfo sel = has_sel ? InfoOf(s, items[size_t(v.item_sel)].id) : ConsumableInfo{};
    for (int r = 0; r < kUseRows; ++r) {
        const int idx = v.item_scroll + r;
        if (idx >= int(items.size())) break;
        const int y = top + r * kItemRowH;
        const int id = items[size_t(idx)].id;
        const ConsumableInfo ci = InfoOf(s, id);
        RowBack(c, a, 30, y, kW - 60, kItemRowH - 6, idx == v.item_sel, r);
        const float alpha = ci.field ? 1.f : .45f;
        if (const Image* ic = a.image(ItemIcon(id))) c.Blit(*ic, 46, y + 8, 62, 62, alpha);
        const std::string qty = std::to_string(items[size_t(idx)].num);
        const float qty_w = a.bold->Measure(qty, 38) + a.regular->Measure("x", 28) + 10;
        c.Text(*a.regular, "x", 1190 - qty_w + 4, y + 52, 28, kInkDim);
        c.Text(*a.bold, qty, 1190, y + 54, 38, ci.field ? kInk : kInkDim, Align::Right);
        ItemTag(c, a, ci, 1190 - qty_w - 18, y + 46);
        c.Text(*a.bold, ItemName(a, id), 128, y + 52, 36, ci.field ? kInk : kInkDim, Align::Left, float(1190 - qty_w - 160 - 128), kShadow);
    }
    if (items.empty()) c.Text(*a.regular, "No items yet.", kW / 2, 420, 34, kInkDim, Align::Center);
    ScrollBar(c, kW - 18, top, kUseRows, kItemRowH, v.item_scroll, int(items.size()));
    // The item's description, or what to do (and why not now)
    const int dy = top + kUseRows * kItemRowH + 4, dh = kUseCardY - dy - 10;
    std::string hint = "Tap a recovery item, then tap who gets it.";
    if (has_sel && !sel.field) hint = "This item can only be used in a battle.";
    const std::string why = ActWhy(v.act, false);
    if (!why.empty() && (!has_sel || sel.field)) hint = why;
    const bool show_desc = has_sel && sel.field && v.act == View::kActNow;
    Description(c, a, 30, dy, kW - 60, dh, show_desc ? ItemDesc(a, items[size_t(v.item_sel)].id) : "", hint);
    // The party: each member's HP and MP, lit when the selected item can be used on them
    const auto members = UseTargets(s);
    const auto boxes = UseCards(members.size());
    const bool armed = has_sel && sel.field && v.act == View::kActNow;
    for (size_t i = 0; i < members.size(); ++i) {
        const UnitState& u = *members[i];
        const CardBox& b = boxes[i];
        const char* why_not = nullptr;
        const bool ok = armed && UseTargetOk(u, sel, &why_not);
        const View::Fx* fx = FxFor(v, u.id, -1);
        Panel(c, a, b.x, b.y, b.w, b.h, ok);
        if (ok) GlowLine(c, b.x + 10, b.y + b.h - 4, b.w - 20, kGold);
        if (fx) c.Frame(b.x, b.y, b.w, b.h, 3, fx->good ? kGold : kFoe);
        const bool ko = u.hp <= 0;
        const float dim = armed && !ok ? .45f : 1.f;
        const int face = std::min(88, b.w / 4);
        c.Disc(float(b.x + 18 + face / 2), float(b.y + 16 + face / 2), face / 2.f + 2, ko ? kFoe : (ok ? kGold : kLineDim));
        if (const Image* f = a.image(Ui("Icon/Unit_Face_Timeline/ICON_UnitFace_unit" + std::to_string(u.id)))) c.BlitFit(*f, b.x + 18, b.y + 16, face, face, dim);
        const int tx = b.x + 30 + face, tw = b.x + b.w - 16 - tx;
        c.Text(*a.bold, UnitName(a, u.id), float(tx), float(b.y + 52), 30, dim < 1 ? kInkDim : kInk, Align::Left, float(tw), kShadow);
        if (ko) c.Text(*a.bold, "KO", float(tx), float(b.y + 86), 24, kFoe);
        else if (u.level > 0) c.Text(*a.regular, "Lv " + std::to_string(u.level), float(tx), float(b.y + 86), 24, kInkDim);
        const int sw = b.w - 36;
        Stat(c, a, b.x + 18, b.y + 116, "HP", u.hp, u.max_hp, kHp, kHpBack, sw);
        Stat(c, a, b.x + 18, b.y + 162, "MP", u.mp, u.max_mp, kMp, kMpBack, sw);
        if (fx) {  // what the item did, over the card
            c.Fill(b.x + 3, b.y + 3, b.w - 6, 62, C(0xC0101E28));
            c.Text(*a.bold, fx->text, float(b.x + b.w / 2), float(b.y + 48), 40, fx->good ? (fx->text.find("MP") != std::string::npos ? kMp : kHp) : kFoeInk, Align::Center, float(b.w - 20), kShadow);
        } else if (v.busy_unit == u.id) {
            c.Fill(b.x + 3, b.y + 3, b.w - 6, 62, C(0xC0101E28));
            c.Text(*a.bold, "\xE2\x80\xA6", float(b.x + b.w / 2), float(b.y + 48), 40, kGold, Align::Center);
        } else if (armed) {
            const char* label = ok ? (sel.party ? "Tap: the whole party" : "Tap to use") : (why_not ? why_not : "");
            c.Text(*a.bold, label, float(b.x + b.w - 16), float(b.y + 86), 22, ok ? kGold : kInkFaint, Align::Right, float(b.w / 2));
        }
    }
}

const char* kSlotNames[4] = {"Weapon", "Armor", "Accessory", "Accessory"};

// The equipment's stat changes against what is worn: Attack, Defense, Magic, Spirit, Divine, Agility, HP, MP
void Lines(const GameState& s, int id, int out[8]) {
    for (int i = 0; i < 8; ++i) out[i] = 0;
    auto it = s.equipment.find(id);
    if (it == s.equipment.end()) return;
    const int* p = it->second.params;
    const int v[8] = {p[5] + p[12], p[6] + p[13], p[7] + p[14], p[8] + p[15], p[16], p[9], p[1], p[2]};
    for (int i = 0; i < 8; ++i) out[i] = v[i];
}
const char* kStatIconNames[6] = {"ICON_Status_attack", "ICON_Status_defense", "ICON_Status_magic", "ICON_Status_mental", "ICON_Status_divine", "ICON_Status_speed"};

// Up to max changes as the game's stat icons with + (green) / - (red) numbers; returns how many were drawn
int Deltas(Canvas& c, const GameState& s, const Art& a, int now_id, int new_id, int x, int y, int max) {
    int was[8], will[8], n = 0;
    Lines(s, now_id, was), Lines(s, new_id, will);
    for (int i = 0; i < 8 && n < max; ++i) {
        const int d = will[i] - was[i];
        if (!d) continue;
        const std::string t = (d > 0 ? "+" : "") + std::to_string(d);
        if (i < 6) {
            if (const Image* ic = a.image(Ui(std::string("Icon/Menu/") + kStatIconNames[i]))) c.Blit(*ic, x, y - 24, 30, 30);
            x += 32;
        } else {
            x += int(c.Text(*a.bold, i == 6 ? "HP" : "MP", float(x), float(y), 20, kInkDim)) + 4;
        }
        x += int(c.Text(*a.bold, t, float(x), float(y), 24, d > 0 ? kHp : kFoe)) + 18;
        ++n;
    }
    return n;
}

int SlotTypeOf(int slot) { return slot == 0 ? 1 : slot == 1 ? 2 : 3; }

// Equipment: a member (the chips), their slots and stats on the left, the pieces for the chosen slot on the right
void Equip(Canvas& c, const GameState& s, const View& v, const Art& a) {
    const auto roster = Roster(s);
    if (roster.empty()) { c.Text(*a.regular, "No party data yet.", kW / 2, 480, 34, kInkDim, Align::Center); return; }
    const int n = std::min<int>(int(roster.size()), 8);
    const int gap = 10, cw = std::min(300, (kW - 60 - gap * (n - 1)) / n);
    for (int i = 0; i < n; ++i) {
        const UnitState& u = *roster[size_t(i)];
        const int x = 30 + i * (cw + gap);
        const bool on = i == v.eq_member;
        Panel(c, a, x, kEqChipY, cw, kEqChipH, on);
        if (on) GlowLine(c, x + 10, kEqChipY + kEqChipH - 4, cw - 20, kGold);
        if (const Image* f = a.image(Ui("Icon/Unit_Face_Timeline/ICON_UnitFace_unit" + std::to_string(u.id)))) c.BlitFit(*f, x + 10, kEqChipY + 12, 60, 60, on ? 1.f : .6f);
        if (cw > 120) c.Text(*a.bold, UnitName(a, u.id), float(x + 78), float(kEqChipY + 54), 26, on ? kInk : kInkDim, Align::Left, float(cw - 86));
    }
    const int mi = std::clamp(v.eq_member, 0, n - 1);
    const UnitState& u = *roster[size_t(mi)];
    // The slots
    for (int sl = 0; sl < 4; ++sl) {
        const int y = kEqSlotY + sl * kEqSlotH;
        const bool on = sl == v.eq_slot;
        Panel(c, a, 30, y, kEqSlotW, kEqSlotH - 8, on);
        const int id = SlotItem(u, sl);
        c.Text(*a.regular, kSlotNames[sl], 48, float(y + 28), 20, on ? kGold : kInkFaint);
        if (id > 0) {
            if (const Image* ic = a.image(ItemIcon(id))) c.Blit(*ic, 46, y + 30, 44, 44);
            c.Text(*a.bold, ItemName(a, id), 102, float(y + 64), 30, kInk, Align::Left, kEqSlotW - 90, kShadow);
        } else {
            c.Text(*a.regular, "Empty", 102, float(y + 64), 28, kInkFaint);
        }
        if (const View::Fx* fx = FxFor(v, u.id, sl)) {
            c.Frame(30, y, kEqSlotW, kEqSlotH - 8, 3, fx->good ? kGold : kFoe);
            c.Text(*a.bold, fx->text, float(30 + kEqSlotW - 16), float(y + 28), 22, fx->good ? kGold : kFoeInk, Align::Right);
        } else if (v.busy_unit == u.id && v.busy_slot == sl) {
            c.Text(*a.bold, "\xE2\x80\xA6", float(30 + kEqSlotW - 20), float(y + 30), 30, kGold, Align::Right);
        }
    }
    // What the piece in the chosen slot does, in the game's words (EquipmentDescription: a passive such as more EXP shows only here)
    const int dy = kEqSlotY + 4 * kEqSlotH + 4, dh = 110;
    {
        Panel(c, a, 30, dy, kEqSlotW, dh);
        c.SetClip(30, dy, kEqSlotW, dh);
        const int worn = SlotItem(u, v.eq_slot);
        const std::string d = worn > 0 ? ItemDesc(a, worn) : "";
        if (!d.empty()) {
            const auto lines = Wrap(*a.regular, d, 23, float(kEqSlotW - 40));
            for (int i = 0; i < int(lines.size()) && i < 3; ++i) {
                const bool last = i == 2 && lines.size() > 3;
                c.Text(*a.regular, last ? lines[size_t(i)] + " ..." : lines[size_t(i)], 50, float(dy + 32 + 29 * i), 23, kInk, Align::Left, float(kEqSlotW - 40));
            }
        } else {
            c.Text(*a.regular, worn > 0 ? "" : std::string("Nothing in the ") + kSlotNames[v.eq_slot] + " slot", 50, float(dy + dh / 2 + 9), 23, kInkFaint, Align::Left, float(kEqSlotW - 40));
        }
        c.ResetClip();
    }
    // The member's stats (as the Party page counts them)
    const int py = dy + dh + 8, ph = kTabBarY - 14 - py;
    Panel(c, a, 30, py, kEqSlotW, ph);
    if (u.stats[0] >= 0) {
        const int rows = 4, rh = (ph - 16) / rows;
        for (int i = 0; i < 8; ++i) {
            const int col = i / rows, row = i % rows;
            const int x = 48 + col * (kEqSlotW / 2), y = py + 8 + row * rh;
            const int k = col == 0 ? (row == 0 ? 6 : row == 1 ? 0 : row == 2 ? 2 : 4) : (row == 0 ? 7 : row == 1 ? 1 : row == 2 ? 3 : 5);
            if (k >= 6) {
                c.Text(*a.bold, k == 6 ? "HP" : "MP", float(x), float(y + rh / 2 + 9), 22, kInkDim);
                c.Text(*a.bold, Num(k == 6 ? u.max_hp : u.max_mp), float(x + kEqSlotW / 2 - 40), float(y + rh / 2 + 10), 28, kInk, Align::Right);
            } else {
                if (const Image* ic = a.image(Ui(std::string("Icon/Menu/") + kStatIconNames[k]))) c.Blit(*ic, x - 4, y + rh / 2 - 18, 34, 34);
                c.Text(*a.bold, std::to_string(u.stats[k]), float(x + kEqSlotW / 2 - 40), float(y + rh / 2 + 10), 28, kInk, Align::Right);
            }
        }
    }
    // The choices for the slot
    const int lx = kEqListX, lw = kW - 30 - lx;
    Panel(c, a, lx, kEqSlotY, lw, kTabBarY - 14 - kEqSlotY);
    c.Text(*a.title, kSlotNames[v.eq_slot], float(lx + 22), float(kEqSlotY + 48), 34, kGold);
    const std::string why = ActWhy(v.act, true);
    c.Text(*a.regular, !why.empty() ? why : v.wearable_known ? "Tap to equip" : "Asking the game\xE2\x80\xA6", float(lx + lw - 22), float(kEqSlotY + 46), 22,
           !why.empty() ? kGold : kInkFaint, Align::Right, float(lw - 200));
    GlowLine(c, lx + 16, kEqSlotY + 60, lw - 32, kLineDim);
    const int now_id = SlotItem(u, v.eq_slot);
    std::vector<int> rows;
    if (SlotTypeOf(v.eq_slot) == 3 && now_id > 0) rows.push_back(-1);  // Remove
    for (int id : v.wearable_known ? v.wearable : EquipChoices(s, v)) rows.push_back(id);
    const float alpha = v.act == View::kActNow && v.wearable_known ? 1.f : .5f;
    for (int r = 0; r < kEqRows; ++r) {
        const int idx = v.eq_scroll + r;
        if (idx >= int(rows.size())) break;
        const int y = kEqListY + r * kEqRowH, id = rows[size_t(idx)];
        const bool worn = id == now_id;
        RowBack(c, a, lx + 10, y, lw - 20, kEqRowH - 6, worn, r);
        if (id < 0) {
            c.Text(*a.bold, "Remove", float(lx + 92), float(y + 62), 30, alpha < 1 ? kInkDim : kInk);
            c.Text(*a.regular, "Take off " + ItemName(a, now_id), float(lx + lw - 30), float(y + 60), 22, kInkFaint, Align::Right, float(lw - 300));
            continue;
        }
        if (const Image* ic = a.image(ItemIcon(id))) c.Blit(*ic, lx + 24, y + 12, 54, 54, alpha);
        // who wears it now: this member (Equipped) or another (their face: equipping it takes it from them, as the game does)
        int other = 0;
        for (auto& m : s.units) {
            if (m.id == u.id || m.join != 0) continue;
            bool on = m.weapon == id || m.armor == id;
            for (int x : m.accessories) on = on || x == id;
            if (on) { other = m.id; break; }
        }
        float right = float(lx + lw - 30);
        if (worn) {
            right -= c.Text(*a.bold, "Equipped", right, float(y + 34), 22, kGold, Align::Right) + 10;
        } else if (other) {
            if (const Image* f = a.image(Ui("Icon/Unit_Face_Timeline/ICON_UnitFace_unit" + std::to_string(other)))) {
                c.Disc(right - 22, float(y + 37), 24, C(0xC0101E28));
                c.BlitFit(*f, int(right - 44), y + 15, 44, 44, alpha);
            }
            right -= 56;
        }
        c.Text(*a.bold, ItemName(a, id), float(lx + 92), float(y + 34), 28, alpha < 1 ? kInkDim : kInk, Align::Left, right - (lx + 92) - 8, kShadow);
        // its stat changes against what is worn (none for the worn piece, or a piece that changes nothing), then what it does in
        // the game's words: on the line below the changes, or on both lines when there are none
        const int changes = worn ? 0 : Deltas(c, s, a, now_id, id, lx + 92, y + 66, 4);
        const std::string d = ItemDesc(a, id);
        const float dw = float(lw - 92 - 40);
        if (changes) {
            c.Text(*a.regular, d, float(lx + 92), float(y + 94), 20, kInkFaint, Align::Left, dw);
        } else {
            const auto lines = Wrap(*a.regular, d.empty() ? (worn ? "" : "No stat change") : d, 20, dw);
            for (int i = 0; i < int(lines.size()) && i < 2; ++i)
                c.Text(*a.regular, i == 1 && lines.size() > 2 ? lines[1] + " ..." : lines[size_t(i)], float(lx + 92), float(y + 66 + 26 * i), 20, kInkFaint, Align::Left, dw);
        }
    }
    if (rows.empty()) c.Text(*a.regular, v.wearable_known ? "Nothing this member can wear here" : "", float(lx + lw / 2), float(kEqListY + 120), 26, kInkDim, Align::Center);
    ScrollBar(c, kW - 44, kEqListY, kEqRows, kEqRowH, v.eq_scroll, int(rows.size()));
}

void Items(Canvas& c, const GameState& s, const View& v, const Art& a) {
    CategoryBar(c, v, a);
    if (v.item_cat == 0) ItemsUse(c, s, v, a);
    else if (v.item_cat == 1) Equip(c, s, v, a);
    else ItemList(c, s, v, a);
}

// ---- Quests ----
// The quest's state as the journal marks it: the tracked one in gold, a completed one dim
void QuestMark(Canvas& c, const Art& a, int cx, int cy, int r, bool main, bool tracked, bool done) {
    const Rgba ring = done ? kInkFaint : main ? kGold : C(0xFF5AA8F0);
    c.Disc(float(cx), float(cy), float(r), tracked ? ring : C(0xA0101E28));
    c.Disc(float(cx), float(cy), float(r - 3), C(0xF0182C3A));
    if (const Image* ic = a.image(Ui("Icon/MAP/ICON_MAP_L_Check"))) c.Blit(*ic, cx - r + 4, cy - r + 4, 2 * r - 8, 2 * r - 8, done ? .4f : 1.f);
}

void Quests(Canvas& c, const GameState& s, const View& v, const Art& a) {
    // Main, Side: the journal's tabs
    const int tw = (kW - 60) / 2;
    for (int i = 0; i < 2; ++i) {
        const int x = 30 + i * tw;
        const bool on = i == v.quest_cat;
        Panel(c, a, x + 4, kQuestCatY, tw - 8, 76, on);
        if (on) GlowLine(c, x + 20, kQuestCatY + 72, tw - 40, i == 0 ? kGold : C(0xFF5AA8F0));
        QuestMark(c, a, x + 50, kQuestCatY + 38, 26, i == 0, true, false);
        const auto list = QuestsOf(s, i);
        int open = 0;
        for (auto* q : list) open += q->prog != 3;
        c.Text(*a.bold, i == 0 ? "Main" : "Side", float(x + 92), float(kQuestCatY + 50), 32, on ? kInk : kInkDim);
        c.Text(*a.regular, std::to_string(open) + " in progress", float(x + tw - 30), float(kQuestCatY + 48), 24, on ? kInkDim : kInkFaint, Align::Right);
    }
    const auto list = QuestsOf(s, v.quest_cat);
    const int lx = 30, lw = kQuestListW;
    for (int r = 0; r < kQuestRows; ++r) {
        const int idx = v.quest_scroll + r;
        if (idx >= int(list.size())) break;
        const Quest& q = *list[size_t(idx)];
        const int y = kQuestListY + r * kQuestRowH;
        RowBack(c, a, lx, y, lw, kQuestRowH - 6, idx == v.quest_sel, r);
        const bool done = q.prog == 3;
        QuestMark(c, a, lx + 38, y + 41, 24, q.cat == 1, q.tracked, done);
        c.Text(*a.bold, q.name.empty() ? "Quest " + std::to_string(q.id) : q.name, float(lx + 76), float(y + 38), 27, done ? kInkDim : kInk, Align::Left, float(lw - 92), kShadow);
        std::string sub = done ? "Complete" : q.tracked ? "Tracked" : !q.place.empty() ? q.place : q.subtitle;
        c.Text(*a.regular, sub, float(lx + 76), float(y + 70), 22, done ? kInkFaint : q.tracked ? kGold : kInkDim, Align::Left, float(lw - 92));
    }
    if (list.empty()) c.Text(*a.regular, v.quest_cat == 0 ? "No story quests yet." : "No side quests yet.", float(lx + lw / 2), 440, 30, kInkDim, Align::Center);
    ScrollBar(c, lx + lw + 4, kQuestListY, kQuestRows, kQuestRowH, v.quest_scroll, int(list.size()));
    // The quest: its name, where, and its story so far (the newest first), with Track at the foot
    const int dx = lx + lw + 20, dw = kW - 30 - dx, dy = kQuestListY, dh = kTabBarY - 14 - dy;
    Panel(c, a, dx, dy, dw, dh);
    if (v.quest_sel < 0 || v.quest_sel >= int(list.size())) {
        c.Text(*a.regular, "Tap a quest to read it and track it.", float(dx + dw / 2), float(dy + dh / 2), 26, kInkFaint, Align::Center, float(dw - 40));
        return;
    }
    const Quest& q = *list[size_t(v.quest_sel)];
    int y = dy + 52;
    for (const auto& line : Wrap(*a.title, q.name, 34, float(dw - 48))) {
        c.Text(*a.title, line, float(dx + 24), float(y), 34, q.cat == 1 ? kGold : C(0xFF8CC4F5), Align::Left, float(dw - 48), kShadow);
        y += 42;
        if (y > dy + 140) break;
    }
    if (!q.subtitle.empty()) { c.Text(*a.bold, q.subtitle, float(dx + 24), float(y + 4), 26, kInk, Align::Left, float(dw - 48)); y += 38; }
    if (!q.place.empty()) {
        if (const Image* ic = a.image(Ui("Icon/MAP/ICON_MAP_L_Town"))) c.Blit(*ic, dx + 22, y - 18, 30, 30);
        c.Text(*a.regular, q.place, float(dx + 60), float(y + 4), 24, kInkDim, Align::Left, float(dw - 84));
        y += 36;
    }
    GlowLine(c, dx + 16, y, dw - 32, kLineDim);
    y += 16;
    const bool track_row = q.prog != 3;
    const int text_bottom = track_row ? kQuestTrackY - 12 : dy + dh - 16;
    c.SetClip(dx, y, dw, text_bottom - y);
    std::vector<std::string> paras(q.log.rbegin(), q.log.rend());
    if (paras.empty() && !q.desc.empty()) paras.push_back(q.desc);
    bool first = true;
    for (const auto& para : paras) {
        for (const auto& line : Wrap(*a.regular, para, 25, float(dw - 48))) {
            if (y + 34 > text_bottom) break;
            y += 32;
            c.Text(*a.regular, line, float(dx + 24), float(y), 25, first ? kInk : kInkDim, Align::Left, float(dw - 48));
        }
        y += 14;
        first = false;
        if (y + 34 > text_bottom) break;
    }
    c.ResetClip();
    if (!track_row) return;
    // Track, as the journal's own action: the game's rule says whether this quest can be tracked
    const int bx = dx + 20, bw = dw - 40, by = kQuestTrackY;
    const View::Fx* fx = FxFor(v, -2, q.id);
    if (!q.can_track) {
        c.Text(*a.regular, q.cat == 1 ? "The story quest is always followed" : "This quest can't be tracked", float(dx + dw / 2), float(by + 46), 24, kInkFaint, Align::Center);
    } else if (q.tracked) {
        c.Fill(bx, by, bw, kQuestTrackH, C(0xE0182C3A));
        c.Frame(bx, by, bw, kQuestTrackH, 2, kGold);
        QuestMark(c, a, bx + 44, by + kQuestTrackH / 2, 24, q.cat == 1, true, false);
        c.Text(*a.bold, "Tracked", float(bx + 84), float(by + 50), 30, kGold);
        c.Text(*a.regular, "Tap to stop tracking", float(bx + bw - 24), float(by + 48), 22, kInkDim, Align::Right);
    } else {
        c.Fill(bx, by, bw, kQuestTrackH, C(0xFF8A6A1E));
        c.Fill(bx + 3, by + 3, bw - 6, kQuestTrackH - 6, kGold);
        c.Text(*a.bold, "Track this quest", float(bx + bw / 2), float(by + 50), 30, kNavy, Align::Center);
    }
    if (fx) c.Text(*a.bold, fx->text, float(bx + bw / 2), float(by - 14), 22, fx->good ? kGold : kFoeInk, Align::Center);
    else if (v.busy_unit == -2) c.Text(*a.bold, "\xE2\x80\xA6", float(bx + bw / 2), float(by - 14), 26, kGold, Align::Center);
    else if (v.quest_act != View::kActNow && q.can_track) c.Text(*a.regular, v.quest_act == View::kActNoHelper ? "The companion's game helper is not running" : "Close the game's menu to change the tracked quest",
                                                       float(bx + bw / 2), float(by - 14), 22, kGold, Align::Center);
}

// ---- Map ----
// The live map is the manifest's map widget, drawn over this frame: the page only gives it a frame and a hint.
void Map(Canvas& c, const GameState& s, const Art& a) {
    const MapState& m = s.map;
    c.Fill(kMapX - 4, kMapY - 4, kMapW + 8, kMapH + 8, C(0xE0101E28));
    c.Frame(kMapX - 4, kMapY - 4, kMapW + 8, kMapH + 8, 2, kLineDim);
    if (!m.valid || (m.texture.empty() && !m.world)) {
        c.Text(*a.regular, "No map for this place.", kW / 2, kMapY + kMapH / 2, 36, kInkDim, Align::Center);
        return;
    }
    c.Text(*a.regular, "Loading map...", kW / 2, kMapY + kMapH / 2, 30, kInkFaint, Align::Center);
}

// ---- Battle ----
// The top half is the battlefield as the game lays it out (enemies left, the party right); the bottom half is the game's
// command window, always in the same place: lit on the player's turn, dimmed while the battle plays out. One layout serves
// drawing and taps (BattleHit).
constexpr int kFieldY = kHeaderH + 10, kWinY = 600, kWinB = kH - 14;  // the whole screen below the header
struct Geom {
    struct Card { int x, y, w, h, unit; bool foe, vision = false; };
    std::vector<Card> cards;
    int colw = 0, xr = 0;
    int lx = 0, ly = 0, lw = 0;           // the list area
    int row_h = 54, rows = 0, cols = 1, colw_list = 0, first = 0;
    int bx = 0, by = 0, bw = 0, bh = 0;   // Back
    struct Slot { int x, y, w, h; };
    std::vector<Slot> finish;             // the resonance wheel's choices (menu.finish order)
};

// A vision equipped to a party member: drawn as a slim card hanging under its owner's, not as another member
bool IsVision(const GameState& s, const BattleUnit& b) {
    if (b.enemy || !b.owner) return false;
    for (auto& o : s.battle)
        if (o.unit == b.owner && !o.enemy && !o.owner) return true;
    return false;
}

constexpr int kVisionIndent = 40;

Geom Layout(const GameState& s) {
    Geom g;
    g.colw = (kW - 72) / 2, g.xr = 24 + g.colw + 24;
    const int y0 = kFieldY + 46, bottom = kWinY - 14, gap = 8, vgap = 4;
    std::vector<int> side[2];
    for (int i = 0; i < int(s.battle.size()); ++i) side[s.battle[size_t(i)].enemy ? 0 : 1].push_back(i);
    for (int k = 0; k < 2; ++k) {
        const int n = int(side[k].size());
        if (!n) continue;
        int nv = 0;
        for (int i : side[k]) nv += IsVision(s, s.battle[size_t(i)]);
        const int nm = n - nv;
        const int h = std::clamp(int((bottom - y0 - gap * std::max(0, nm - 1) - vgap * nv) / (nm + 0.5 * nv)), 40, 100);
        const int hv = std::clamp(h / 2, 34, 50);
        int y = y0;
        bool first = true;
        for (int i : side[k]) {
            const bool vis = IsVision(s, s.battle[size_t(i)]);
            y += first ? 0 : vis ? vgap : gap;
            const int ch = vis ? hv : h;
            if (y + ch > bottom) break;
            const int x = k == 0 ? 24 : g.xr;
            g.cards.push_back({vis ? x + kVisionIndent : x, y, vis ? g.colw - kVisionIndent : g.colw, ch, i, k == 0, vis});
            y += ch;
            first = false;
        }
    }
    g.lx = 404, g.ly = kWinY + 74, g.lw = kW - 24 - 20 - g.lx;
    g.rows = (kWinB - 14 - g.ly) / g.row_h;
    const int n = int(s.menu.entries.size());
    g.cols = n > g.rows ? 2 : 1;
    g.colw_list = (g.lw - (g.cols - 1) * 16) / g.cols;
    const int page = g.rows * g.cols;
    g.first = std::clamp(s.menu.cursor - (s.menu.cursor % g.rows) - (g.cols - 1) * g.rows, 0, std::max(0, n - page));
    g.first -= g.first % g.rows;
    g.bw = 150, g.bh = 50, g.bx = kW - 24 - 20 - g.bw, g.by = kWinY + 14;
    // The resonance wheel: up on top, down at the bottom, left and right beside the middle, as the game's diamond
    const auto& fin = s.menu.finish;
    if (s.menu.state == 2 && !fin.empty()) {
        const int top = g.ly - 4, bot = kWinB - 16, sw = std::min(330, g.lw / 2 - 12), sh = std::min(84, (bot - top - 16) / 3);
        const int cx = g.lx + g.lw / 2, cy = (top + bot) / 2;
        int spare = 0;  // a choice without a direction takes the next free place
        bool used[4] = {};
        for (auto& f : fin) if (f.dir >= 0) used[f.dir] = true;
        for (auto& f : fin) {
            int d = f.dir;
            if (d < 0) { while (spare < 4 && used[spare]) ++spare; d = spare < 4 ? spare : 3; if (spare < 4) used[spare] = true; }
            Geom::Slot sl{0, 0, sw, sh};
            if (d == 2) sl.x = cx - sw / 2, sl.y = top;
            else if (d == 3) sl.x = cx - sw / 2, sl.y = bot - sh;
            else if (d == 0) sl.x = g.lx, sl.y = cy - sh / 2;
            else sl.x = g.lx + g.lw - sw, sl.y = cy - sh / 2;
            g.finish.push_back(sl);
        }
    }
    return g;
}

bool In(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && y >= ry && x < rx + rw && y < ry + rh; }

// The game's icon for a command (its battle menu icons), else the item's own
std::string ActionIcon(const ActionRef& r) {
    if (r.cat == 2) return ItemIcon(r.id);
    const int id = r.id;
    if (r.cat == 1 && id < 1000) {  // a command list
        if (id == 7) return Ui("Icon/Menu/ICON_Battle_Item");
        if (id == 10) return Ui("Icon/Menu/ICON_Battle_Limit");
        if (id == 5 || id == 6) return Ui("Icon/Menu/ICON_Battle_summon");
        return Ui("Icon/Menu/ICON_Battle_Skill");
    }
    if (id == 400010 || id == 400640) return Ui("Icon/Menu/ICON_Battle_Guard");
    if (id == 400020) return Ui("Icon/Menu/ICON_Battle_Escape");
    if (id >= 401000 && id < 500000 && id % 1000 == 10) return Ui("Icon/Menu/ICON_Battle_Attack");
    if (id >= 100000 && id < 200000) return Ui("Icon/Menu/ICON_Battle_Item");  // an item used as a skill
    return Ui("Icon/Menu/ICON_Battle_Fight");
}

void UnitCard(Canvas& c, const GameState& s, const Art& a, const Geom::Card& cd, const std::string& name) {
    const BattleUnit& b = s.battle[size_t(cd.unit)];
    const int x = cd.x, y = cd.y, w = cd.w, h = cd.h;
    const bool foe = cd.foe, down = b.hp == 0;
    auto has = [&](const std::vector<uint64_t>& v) { return std::find(v.begin(), v.end(), b.unit) != v.end(); };
    const bool choosing = s.menu.valid && s.menu.state == 1 && b.unit;
    const bool targeted = choosing && has(s.menu.targets);   // under the game's target pointer
    const bool reached = choosing && !targeted && has(s.menu.sub);  // also hit (an area action)
    const bool acting = !foe && s.menu.open && b.id == s.menu.actor;
    const Rgba accent = foe ? kFoe : kLine;
    if (cd.vision) {  // a vision: hangs under its owner's card, joined by a line
        const int lx = x - kVisionIndent / 2;
        c.Fill(lx, y - 6, 2, h / 2 + 6, kGold);
        c.Fill(lx, y + h / 2 - 1, kVisionIndent / 2, 2, kGold);
        c.Fill(x, y, w, h, targeted ? C(0xE0305868) : reached ? C(0xD0382E3C) : C(0xB0101C26));
        c.Fill(x, y, 3, h, down ? kInkFaint : kGold);
        if (targeted) c.Frame(x - 3, y - 3, w + 6, h + 6, 2, C(0x80FFFFFF));
        c.Frame(x, y, w, h, targeted ? 3 : reached ? 2 : 1, targeted ? kInk : reached ? kLine : C(0x60E6C36B));
        int tx = x + 14;
        if (targeted || reached) {
            if (const Image* rt = a.image(Ui("Menu/T_Btl_Libra_Targeting"))) c.BlitFit(*rt, x + 4, y + h / 2 - 18, 36, 36, targeted ? 1.f : 0.45f);
            tx = x + 44;
        }
        const float tagw = a.bold->Measure("VISION", 17) + 14;
        c.Fill(tx, y + h / 2 - 13, int(tagw), 24, C(0xC0302410));
        c.Frame(tx, y + h / 2 - 13, int(tagw), 24, 1, kGold);
        c.Text(*a.bold, "VISION", tx + tagw / 2, y + h / 2 + 5, 17, kGold, Align::Center);
        const int nx = tx + int(tagw) + 12, right = x + w - 14;
        const std::string val = down ? "KO" : Num(b.hp) + (b.max_hp > 0 ? " / " + Num(b.max_hp) : "");
        const float vw = a.bold->Measure(val, 22);
        const int base = y + h / 2 + (h >= 44 ? 2 : 8);
        c.Text(*a.bold, name, nx, base, 24, down ? kInkFaint : kInk, Align::Left, right - nx - vw - 12, kShadow);
        c.Text(*a.bold, val, right, base, 22, down ? kInkFaint : kInkDim, Align::Right, 0, kShadow);
        if (h >= 44) Bar(c, nx, base + 7, right - nx, 5, b.hp, b.max_hp > 0 ? b.max_hp : b.hp, kHp, kHpBack);
        return;
    }
    c.Fill(x, y, w, h, targeted ? (foe ? C(0xE0503040) : C(0xE0305868)) : reached ? C(0xD0382E3C) : acting ? C(0xE02A5568) : C(0xC0182C3A));
    c.GradientV(x, y, w, h / 2, C(0x22FFFFFF), C(0x00FFFFFF));
    if (!foe) {  // the character's art, fading in from the right
        if (const Image* art = a.image(Ui("Picture/Char/T_Chara_Button_" + std::to_string(b.id)))) {
            c.SetClip(x + 1, y + 1, w - 2, h - 2);
            c.SetFadeH(x + w - 60, x + w - 300);
            c.BlitFit(*art, x + w - 300, y + 1, 298, h - 2, down ? 0.3f : 0.85f, true);
            c.SetFadeH(0, 0);
            c.ResetClip();
        }
    }
    c.Fill(x, y, 4, h, down ? kInkFaint : acting ? kGold : accent);
    if (targeted) c.Frame(x - 3, y - 3, w + 6, h + 6, 2, C(0x80FFFFFF));  // the pointer's glow
    c.Frame(x, y, w, h, targeted || acting ? 3 : reached ? 2 : 1, targeted ? kInk : reached ? (foe ? kFoe : kLine) : acting ? kGold : C(0x405EC4E6));
    int tx = x + 18;
    if (targeted || reached) {  // the game's targeting reticle
        if (const Image* rt = a.image(Ui("Menu/T_Btl_Libra_Targeting"))) c.BlitFit(*rt, x + 6, y + h / 2 - 26, 52, 52, targeted ? 1.f : 0.45f);
        tx = x + 64;
    }
    const int tw = x + w - (foe ? 18 : 150) - tx;
    const bool mp_bar = !foe && b.mp >= 0 && b.max_mp > 0 && h >= 62;
    const int fs = h >= 80 ? 32 : 28;
    const int base = y + (h - (mp_bar ? 52 : 38)) / 2 + fs - 6;
    const std::string val = down ? "KO" : Num(b.hp) + (b.max_hp > 0 && !foe ? " / " + Num(b.max_hp) : "");
    const float vw = (down ? a.regular : a.bold)->Measure(val, 26);
    // Staggered (the break gauge emptied): the game's tag, after the name
    const bool stag = foe && b.staggered && !down;
    const float sw = stag ? a.bold->Measure("STAGGERED", 18) + 16 : 0;
    const float nmax = tw - vw - 14 - (stag ? sw + 10 : 0);
    c.Text(*a.bold, name, tx, base, fs, down ? kInkFaint : foe ? kFoeInk : kInk, Align::Left, nmax, kShadow);
    if (stag) {
        const int sx = tx + int(std::min(a.bold->Measure(name, float(fs)), nmax)) + 10, sy = base - 24;
        c.Fill(sx, sy, int(sw), 28, C(0xD0201810));
        c.Frame(sx, sy, int(sw), 28, 2, kGold);
        c.Text(*a.bold, "STAGGERED", sx + sw / 2, sy + 21, 18, kGold, Align::Center);
    }
    c.Text(down ? *a.regular : *a.bold, val, tx + tw, base, 26, down ? kInkFaint : kInk, Align::Right, 0, kShadow);
    Bar(c, tx, base + 9, tw, 9, b.hp, b.max_hp > 0 ? b.max_hp : b.hp, foe ? kFoe : kHp, foe ? kFoeBack : kHpBack);
    if (mp_bar) Bar(c, tx, base + 22, tw, 6, b.mp, b.max_mp, kMp, kMpBack);
}

void Button(Canvas& c, const Art& a, int x, int y, int w, int h, const std::string& label, bool lit, int px = 28) {
    c.Fill(x, y, w, h, lit ? C(0xE0244A5C) : C(0xC0101E28));
    c.GradientV(x, y, w, h / 2, C(0x26FFFFFF), C(0x00FFFFFF));
    c.Frame(x, y, w, h, 2, lit ? kLine : kLineDim);
    c.Text(*a.bold, label, x + w / 2, y + h / 2 + px * 0.36f, float(px), lit ? kInk : kInkDim, Align::Center, float(w - 12), kShadow);
}

// Two of the same enemy: "Goblin A", "Goblin B", as the game tells them apart
std::string BattleName(const GameState& s, const Art& a, int i) {
    const BattleUnit& b = s.battle[size_t(i)];
    std::string name = UnitName(a, b.id);
    int same = 0, nth = 0;
    for (int j = 0; j < int(s.battle.size()); ++j)
        if (s.battle[size_t(j)].id == b.id && s.battle[size_t(j)].enemy == b.enemy) ++same, nth += j < i;
    if (same > 1 && nth < 26) name += std::string(" ") + char('A' + nth);
    return name;
}

void CommandWindow(Canvas& c, const GameState& s, const Art& a, const Geom& g) {
    const BattleMenu& mn = s.menu;
    const bool live = mn.valid;
    // The game's battle window
    const int wx = 24, ww = kW - 48, wh = kWinB - kWinY;
    if (const Image* win = a.image(Ui(live ? "Menu/T_BtlWindow_Active" : "Menu/T_BtlWindow_Inactive"))) c.NineSlice(*win, wx, kWinY, ww, wh, 16, 1.f, 0.96f);
    else Panel(c, a, wx, kWinY, ww, wh, live);
    c.Frame(wx, kWinY, ww, wh, 2, live ? kLine : kLineDim);
    // Left: whose turn, with their portrait and HP / MP
    const BattleUnit* actor = nullptr;
    for (auto& b : s.battle)
        if (!b.enemy && b.id == mn.actor) actor = &b;
    const int px = wx + 16, pw = g.lx - 20 - px;
    if (actor && mn.open) {
        if (const Image* art = a.image(Ui("Picture/Char/T_Chara_Button_" + std::to_string(actor->id)))) {
            c.SetClip(px, kWinY + 10, pw, wh - 20);
            c.SetFadeH(px + pw - 40, px + pw);
            c.BlitFit(*art, px - 40, kWinY + 10, pw + 80, 230, 0.95f, true);
            c.SetFadeH(0, 0);
            c.ResetClip();
        }
        c.GradientV(px, kWinY + 150, pw, 90, C(0x00101E28), C(0xE0101E28));
        c.Text(*a.title, UnitName(a, actor->id), px + 10, kWinY + 228, 40, kInk, Align::Left, pw - 20, kShadow);
        Stat(c, a, px + 10, kWinY + 262, "HP", actor->hp, actor->max_hp, kHp, kHpBack, pw - 24);
        Stat(c, a, px + 10, kWinY + 318, "MP", actor->mp, actor->max_mp, kMp, kMpBack, pw - 24);
    } else {
        c.Text(*a.title, live ? "Command" : "Battle", px + 10, kWinY + 70, 40, live ? kInk : kInkFaint, Align::Left, pw - 20, kShadow);
    }
    c.GradientV(g.lx - 12, kWinY + 16, 2, wh - 32, C(0x805EC4E6), C(0x105EC4E6));
    if (!live) {  // the battle is playing out: the window waits, as the game's does
        c.Text(*a.regular, "Waiting for your turn...", g.lx + g.lw / 2, kWinY + wh / 2 + 10, 32, kInkFaint, Align::Center);
        return;
    }
    // Title: the path to this list, or the target choice; Back on the right
    std::string title;
    std::string icon = Ui("Icon/Menu/ICON_Battle_Fight");
    for (auto& p : mn.path) title += (title.empty() ? "" : "  \xE2\x80\xBA  ") + ActionName(a, p), icon = ActionIcon(p);
    if (mn.state == 1) {  // the target choice, made on the controller as in the game: the action, then who the pointer is on
        title = ActionName(a, mn.path.back()) + "  \xE2\x80\xBA  ";
        std::string who;
        for (int i = 0; i < int(s.battle.size()); ++i)
            if (std::find(mn.targets.begin(), mn.targets.end(), s.battle[size_t(i)].unit) != mn.targets.end())
                who += (who.empty() ? "" : ", ") + BattleName(s, a, i);
        if (!who.empty() && !mn.sub.empty()) who += " +" + std::to_string(mn.sub.size());
        title += who.empty() ? "Choose a target" : who;
        icon = Ui("Menu/T_Btl_Libra_Targeting");
    }
    if (mn.state == 2) title = "Resonance", icon = Ui("Icon/Menu/ICON_Battle_Limit");
    if (title.empty()) title = "Command";
    if (const Image* ic = a.image(icon)) c.Blit(*ic, g.lx, kWinY + 18, 44, 44);
    c.Text(*a.bold, title, g.lx + 56, kWinY + 52, 32, mn.state == 1 ? kGold : kInk, Align::Left, g.bx - g.lx - 70, kShadow);
    Button(c, a, g.bx, g.by, g.bw, g.bh, "Back", false, 26);
    GlowLine(c, g.lx, kWinY + 68, g.lw, kLineDim);
    if (mn.state == 2) {  // the resonance wheel: each choice by its d-pad direction
        for (size_t i = 0; i < mn.finish.size() && i < g.finish.size(); ++i) {
            const auto& f = mn.finish[i];
            const auto& sl = g.finish[i];
            c.Fill(sl.x, sl.y, sl.w, sl.h, f.enable ? C(0xE0183848) : C(0xA0101820));
            c.GradientV(sl.x, sl.y, sl.w, sl.h / 2, C(0x22FFFFFF), C(0x00FFFFFF));
            c.Frame(sl.x, sl.y, sl.w, sl.h, 2, f.enable ? kLine : kLineDim);
            // the game's round d-pad badge
            const float r = std::min(26, sl.h / 2 - 6), bx = sl.x + 12 + r, by = sl.y + sl.h / 2.f;
            c.Disc(bx, by, r, f.enable ? C(0xFF0E2A3C) : C(0xFF1A2228));
            c.Disc(bx, by, r - 3, f.enable ? C(0xFFE6F4FA) : C(0xFF5A6670));
            c.Disc(bx, by, r - 6, f.enable ? C(0xFF0E2A3C) : C(0xFF1A2228));
            const float t = r * 0.42f;
            const Rgba tc = f.enable ? kInk : kInkFaint;
            if (f.dir == 2) c.Triangle(bx, by - t, bx - t, by + t * 0.7f, bx + t, by + t * 0.7f, tc);
            else if (f.dir == 3) c.Triangle(bx, by + t, bx - t, by - t * 0.7f, bx + t, by - t * 0.7f, tc);
            else if (f.dir == 0) c.Triangle(bx - t, by, bx + t * 0.7f, by - t, bx + t * 0.7f, by + t, tc);
            else if (f.dir == 1) c.Triangle(bx + t, by, bx - t * 0.7f, by - t, bx - t * 0.7f, by + t, tc);
            const int tx = int(bx + r + 12), tw = sl.x + sl.w - 10 - tx;
            std::string who;
            for (int k = 0; k < int(s.battle.size()); ++k)
                if (f.unit && s.battle[size_t(k)].unit == f.unit) who = BattleName(s, a, k);
            const ActionRef sk{1, f.skill};
            c.Text(*a.bold, ActionName(a, sk), tx, sl.y + sl.h / 2 + (who.empty() ? 10 : -2), 28, f.enable ? kInk : kInkFaint, Align::Left, tw, kShadow);
            if (!who.empty()) c.Text(*a.regular, who, tx, sl.y + sl.h / 2 + 26, 22, f.enable ? kGold : kInkFaint, Align::Left, tw);
        }
        return;
    }
    // The list, as the game's: its icon per command, the cursor bar on the focused one
    const int n = int(mn.entries.size());
    for (int k = 0; k < g.rows * g.cols && g.first + k < n; ++k) {
        const int i = g.first + k, col = k / g.rows, row = k % g.rows;
        const int x = g.lx + col * (g.colw_list + 16), y = g.ly + row * g.row_h;
        const ActionRef& e = mn.entries[size_t(i)];
        const bool cur = i == mn.cursor;
        if (cur) {
            if (const Image* hl = a.image(Ui("Menu/T_Btl_CommandListSelect"))) c.Blit(*hl, x, y + 2, g.colw_list, g.row_h - 4);
            else c.Fill(x, y + 2, g.colw_list, g.row_h - 4, C(0x80B04AA0));
            if (const Image* st = a.image(Ui("Menu/T_SymbolArrow00"))) c.Blit(*st, x - 14, y + g.row_h / 2 - 18, 36, 36);
        }
        if (const Image* ic = a.image(ActionIcon(e))) c.Blit(*ic, x + 22, y + 6, g.row_h - 12, g.row_h - 12);
        std::string right;
        if (e.cat == 2) {
            for (auto& it : s.inventory)
                if (it.id == e.id) right = "x" + std::to_string(it.num);
        } else if (e.cat == 1 && e.id < 1000) {
            right = "\xE2\x80\xBA";
        }
        const float rw = right.empty() ? 0 : a.bold->Measure(right, 26) + 10;
        const int tx = x + 22 + g.row_h;
        c.Text(*a.bold, ActionName(a, e), tx, y + g.row_h / 2 + 11, 30, kInk, Align::Left, x + g.colw_list - 16 - rw - tx, kShadow);
        if (!right.empty()) c.Text(*a.bold, right, x + g.colw_list - 14, y + g.row_h / 2 + 10, 26, kInkDim, Align::Right);
    }
    if (mn.state == 1) c.Fill(g.lx - 4, g.ly, g.lw + 8, g.rows * g.row_h, C(0x70081018));  // the choice moved to the field above
    if (n > g.rows * g.cols) {  // more below or above
        const int page = g.rows * g.cols, track = g.rows * g.row_h, th = std::max(30, track * page / n);
        const int ty = g.ly + (track - th) * g.first / std::max(1, n - page);
        c.Fill(kW - 24 - 12, g.ly, 4, track, C(0x40FFFFFF));
        c.Fill(kW - 24 - 12, ty, 4, th, kLine);
    }
}

void Battle(Canvas& c, const GameState& s, const Art& a) {
    if (!s.in_battle) {
        c.Text(*a.title, "No battle in progress", kW / 2, 300, 48, kInkDim, Align::Center);
        int y = 380;
        for (int id : s.party) {
            const UnitState* u = nullptr;
            for (auto& x : s.units)
                if (x.id == id) u = &x;
            if (!u) continue;
            Panel(c, a, 200, y, kW - 400, 120);
            c.Text(*a.bold, UnitName(a, u->id), 230, y + 50, 36, kInk, Align::Left, 300, kShadow);
            Stat(c, a, 560, y + 26, "HP", u->hp, u->max_hp, kHp, kHpBack, 440);
            Stat(c, a, 560, y + 76, "MP", u->mp, u->max_mp, kMp, kMpBack, 440);
            y += 136;
        }
        return;
    }
    const Geom g = Layout(s);
    int count[2] = {};
    for (auto& b : s.battle) count[b.enemy ? 0 : 1] += !IsVision(s, b);
    for (int k = 0; k < 2; ++k) {
        const int x = k == 0 ? 24 : g.xr;
        const Rgba accent = k == 0 ? kFoe : kLine;
        c.Text(*a.title, k == 0 ? "Enemies" : "Party", x + 6, kFieldY + 30, 32, kInk, Align::Left, 0, kShadow);
        c.Text(*a.bold, std::to_string(count[k]), x + g.colw - 6, kFieldY + 30, 26, kInkDim, Align::Right);
        c.GradientH(x, kFieldY + 40, g.colw, 2, accent, Rgba{accent.r, accent.g, accent.b, 0});
    }
    for (auto& cd : g.cards) UnitCard(c, s, a, cd, BattleName(s, a, cd.unit));
    CommandWindow(c, s, a, g);
}

} // namespace

std::vector<ItemStack> ItemsOf(const GameState& s, int cat) {
    std::vector<ItemStack> out;
    for (auto& i : s.inventory)
        if (i.num > 0 && Category(i.id) == cat) out.push_back(i);
    std::stable_sort(out.begin(), out.end(), [](const ItemStack& a, const ItemStack& b) { return a.id < b.id; });
    return out;
}

std::string ActionName(const Art& a, const ActionRef& r) {
    std::string n;
    const std::string id = std::to_string(r.id);
    if (r.cat == 2) return ItemName(a, r.id);
    if (r.cat == 1) n = a.text("ST_SkillName", "SkillName" + id);
    if (n.empty()) n = a.text("ST_CommandSkillName", "CommandSkillName" + id);
    if (n.empty() && r.cat == 0) n = a.text("ST_SkillName", "SkillName" + id);
    return n.empty() ? "#" + id : n;
}

BattleTap BattleHit(const GameState& s, int x, int y) {
    BattleTap t;
    if (!s.in_battle) return t;
    const Geom g = Layout(s);
    const BattleMenu& mn = s.menu;
    if (mn.valid) {
        if (In(x, y, g.bx - 8, g.by - 8, g.bw + 16, g.bh + 16)) return t.kind = BattleTap::kBack, t;
        if (mn.state == 2)
            for (size_t i = 0; i < mn.finish.size() && i < g.finish.size(); ++i) {
                const auto& sl = g.finish[i];
                if (mn.finish[i].enable && mn.finish[i].dir >= 0 && In(x, y, sl.x, sl.y, sl.w, sl.h)) return t.kind = BattleTap::kFinish, t.index = int(i), t;
            }
        if (mn.state == 0)
            for (int k = 0; k < g.rows * g.cols && g.first + k < int(mn.entries.size()); ++k) {
                const int col = k / g.rows, row = k % g.rows;
                if (In(x, y, g.lx + col * (g.colw_list + 16), g.ly + row * g.row_h, g.colw_list, g.row_h)) return t.kind = BattleTap::kEntry, t.index = g.first + k, t;
            }
    }
    for (auto& cd : g.cards)
        if (In(x, y, cd.x, cd.y - 4, cd.w, cd.h + 8)) return t.kind = BattleTap::kUnit, t.index = cd.unit, t;
    return t;
}

int SlotType(int slot) { return SlotTypeOf(slot); }
int SlotItem(const UnitState& u, int slot) {
    if (slot == 0) return u.weapon;
    if (slot == 1) return u.armor;
    const size_t i = size_t(slot - 2);
    return i < u.accessories.size() ? u.accessories[i] : -1;
}

std::vector<int> EquipChoices(const GameState& s, const View& v) {
    std::vector<int> out;
    const int type = SlotTypeOf(v.eq_slot);
    for (auto& i : s.inventory) {
        if (i.num <= 0) continue;
        auto e = s.equipment.find(i.id);
        if (e != s.equipment.end() && e->second.type == type) out.push_back(i.id);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.size() > 256) out.resize(256);
    return out;
}

std::vector<const UnitState*> UseTargets(const GameState& s) {
    std::vector<const UnitState*> out;
    for (int id : s.party)
        for (auto& u : s.units)
            if (u.id == id && out.size() < 4) out.push_back(&u);
    return out;
}

bool UseTargetOk(const UnitState& u, const ConsumableInfo& c, const char** why) {
    auto no = [&](const char* w) { if (why) *why = w; return false; };
    if (!c.field) return no("Battle only");
    const bool ko = u.hp <= 0;
    if (c.kind == ConsumableInfo::kRevive) return ko ? true : no("Not KO");
    if (ko) return no("KO");
    if (c.kind == ConsumableInfo::kHp && u.max_hp > 0 && u.hp >= u.max_hp) return no("HP is full");
    if (c.kind == ConsumableInfo::kMp && u.max_mp > 0 && u.mp >= u.max_mp) return no("MP is full");
    return true;
}

int ListRows(const View& v) { return v.tab == kTabQuests ? kQuestRows : v.item_cat == 0 ? kUseRows : v.item_cat == 1 ? kEqRows : kItemRows; }

int ListCount(const GameState& s, const View& v) {
    if (v.tab == kTabQuests) return int(QuestsOf(s, v.quest_cat).size());
    if (v.item_cat != 1) return int(ItemsOf(s, v.item_cat).size());
    const auto roster = Roster(s);
    if (roster.empty()) return 0;
    const UnitState& u = *roster[size_t(std::clamp(v.eq_member, 0, int(roster.size()) - 1))];
    const int extra = SlotTypeOf(v.eq_slot) == 3 && SlotItem(u, v.eq_slot) > 0 ? 1 : 0;
    return extra + int(v.wearable_known ? v.wearable.size() : EquipChoices(s, v).size());
}

ItemsTap ItemsHit(const GameState& s, const View& v, int x, int y) {
    ItemsTap t;
    if (v.item_cat == 0) {
        const int n = int(ItemsOf(s, 0).size());
        if (y >= kItemListY && y < kItemListY + kUseRows * kItemRowH) {
            const int idx = v.item_scroll + (y - kItemListY) / kItemRowH;
            if (idx < n) t.kind = ItemsTap::kRow, t.index = idx;
            return t;
        }
        const auto members = UseTargets(s);
        const auto boxes = UseCards(members.size());
        for (size_t i = 0; i < boxes.size(); ++i)
            if (In(x, y, boxes[i].x, boxes[i].y, boxes[i].w, boxes[i].h)) return t.kind = ItemsTap::kTarget, t.id = members[i]->id, t;
        return t;
    }
    if (v.item_cat == 1) {
        const auto roster = Roster(s);
        const int n = std::min<int>(int(roster.size()), 8);
        if (!n) return t;
        const int gap = 10, cw = std::min(300, (kW - 60 - gap * (n - 1)) / n);
        if (y >= kEqChipY && y < kEqChipY + kEqChipH) {
            for (int i = 0; i < n; ++i)
                if (x >= 30 + i * (cw + gap) && x < 30 + i * (cw + gap) + cw) return t.kind = ItemsTap::kMember, t.index = i, t;
            return t;
        }
        if (x >= 30 && x < 30 + kEqSlotW && y >= kEqSlotY && y < kEqSlotY + 4 * kEqSlotH) return t.kind = ItemsTap::kSlot, t.index = (y - kEqSlotY) / kEqSlotH, t;
        if (x >= kEqListX && y >= kEqListY && y < kEqListY + kEqRows * kEqRowH) {
            const UnitState& u = *roster[size_t(std::clamp(v.eq_member, 0, n - 1))];
            std::vector<int> rows;
            if (SlotTypeOf(v.eq_slot) == 3 && SlotItem(u, v.eq_slot) > 0) rows.push_back(-1);
            for (int id : v.wearable_known ? v.wearable : EquipChoices(s, v)) rows.push_back(id);
            const int idx = v.eq_scroll + (y - kEqListY) / kEqRowH;
            if (idx < int(rows.size())) {
                if (rows[size_t(idx)] < 0) t.kind = ItemsTap::kRemove;
                else t.kind = ItemsTap::kChoice, t.id = rows[size_t(idx)];
            }
        }
        return t;
    }
    if (y >= kItemListY && y < kItemListY + kItemRows * kItemRowH) {
        const int idx = v.item_scroll + (y - kItemListY) / kItemRowH;
        if (idx < int(ItemsOf(s, v.item_cat).size())) t.kind = ItemsTap::kRow, t.index = idx;
    }
    return t;
}

std::vector<const Quest*> QuestsOf(const GameState& s, int cat) {
    std::vector<const Quest*> out;
    for (auto& q : s.quests)
        if (q.prog >= 1 && (cat == 0 ? q.cat == 1 : q.cat >= 2)) out.push_back(&q);
    std::stable_sort(out.begin(), out.end(), [](const Quest* a, const Quest* b) {
        const bool da = a->prog == 3, db = b->prog == 3;
        if (da != db) return !da;                       // in progress first
        if (a->tracked != b->tracked) return a->tracked;  // the tracked one on top
        return da ? a->id > b->id : a->id < b->id;      // the newest completed first
    });
    return out;
}

QuestsTap QuestsHit(const GameState& s, const View& v, int x, int y) {
    QuestsTap t;
    if (y >= kQuestCatY && y < kQuestCatY + 76) return t.kind = QuestsTap::kCat, t.index = x < kW / 2 ? 0 : 1, t;
    const auto list = QuestsOf(s, v.quest_cat);
    if (x >= 30 && x < 30 + kQuestListW && y >= kQuestListY && y < kQuestListY + kQuestRows * kQuestRowH) {
        const int idx = v.quest_scroll + (y - kQuestListY) / kQuestRowH;
        if (idx < int(list.size())) t.kind = QuestsTap::kRow, t.index = idx;
        return t;
    }
    if (x >= 30 + kQuestListW + 20 && y >= kQuestTrackY && y < kQuestTrackY + kQuestTrackH && v.quest_sel >= 0 && v.quest_sel < int(list.size())) {
        const Quest& q = *list[size_t(v.quest_sel)];
        if (q.can_track && q.prog != 3) t.kind = QuestsTap::kTrack;
    }
    return t;
}

std::vector<const UnitState*> Roster(const GameState& s) {
    std::vector<const UnitState*> out;
    for (int id : s.party)
        for (auto& u : s.units)
            if (u.id == id) out.push_back(&u);
    for (auto& u : s.units)
        if (u.join == 0 && std::find(s.party.begin(), s.party.end(), u.id) == s.party.end()) out.push_back(&u);
    return out;
}

void ComposePage(Canvas& c, const GameState& s, const View& v, const Art& a) {
    g_icon_state = &s;
    Background(c, a);
    static const char* kTitles[kTabCount + 1] = {"Map", "Party", "Items", "Quests", "Battle"};
    if (!s.in_game || !s.save_ok) {  // title screen, boot or loading: the game's logo until a game is loaded
        c.GradientV(0, 0, kW, kH, C(0x600A131B), C(0xB00A131B));
        const Image* emblem = a.image(Ui("Title/T_TitleLogoImageRGB"));
        const Image* text = a.image(Ui("Title/T_TitleLogoTextAdd"));
        const Image* line = a.image(Ui("Title/T_TitleLogoLineAdd"));
        if (emblem) c.BlitFit(*emblem, 170, 120, 900, 450);
        if (text) c.BlitFit(*text, 120, 560, 1000, 250);
        if (line) c.BlitFit(*line, 220, 790, 800, 26, 0.8f);
        if (!emblem && !text) c.Text(*a.title, "FINAL FANTASY RESONANCE", kW / 2, 520, 64, kInk, Align::Center);
        const std::string msg = s.status == "Unsupported game version" ? s.status : "Load a game to see your party";
        c.Text(*a.regular, msg, kW / 2, 900, 30, kInkDim, Align::Center, 0, kShadow);
        return;
    }
    Header(c, s, a, v.tab == kTabMap && !s.map.name.empty() ? s.map.name : kTitles[v.tab]);
    switch (v.tab) {
    case kTabMap: Map(c, s, a); break;
    case kTabParty: {
        auto roster = Roster(s);
        if (v.member >= 0 && v.member < int(roster.size())) PartyDetail(c, s, a, *roster[size_t(v.member)]);
        else PartyList(c, s, a);
        break;
    }
    case kTabItems: Items(c, s, v, a); break;
    case kTabQuests: Quests(c, s, v, a); break;
    default: Battle(c, s, a); return;  // a battle has the whole screen
    }
    TabBar(c, a, v.tab, v.map_ok);
}

} // namespace ffr
