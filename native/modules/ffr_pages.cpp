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
    const char* names[kTabCount] = {"Map", "Party", "Items", "Battle"};
    const std::string icons[kTabCount] = {Ui("Icon/MAP/ICON_MAP_L_Town"), Ui("Icon/Menu/ICON_Battle_Fight"), Ui("Icon/Menu/ICON_Battle_Item"),
                                         Ui("Icon/Menu/ICON_Battle_Attack")};
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
void Items(Canvas& c, const GameState& s, const View& v, const Art& a) {
    const char* cats[4] = {"Items", "Equipment", "Key Items", "Artifacts"};
    const std::string icons[4] = {Ui("Icon/Menu/ICON_Battle_Item"), Ui("Icon/Menu/ICON_Item_Brade"), Ui("Icon/Menu/ICON_Item_Key"), Ui("Icon/Menu/ICON_Item_Kira")};
    const int tw = (kW - 60) / 4;
    for (int i = 0; i < 4; ++i) {
        const int x = 30 + i * tw;
        Panel(c, a, x + 4, 128, tw - 8, 76, i == v.item_cat);
        if (const Image* ic = a.image(icons[i])) c.Blit(*ic, x + 20, 140, 52, 52, i == v.item_cat ? 1.f : .55f);
        c.Text(*a.bold, cats[i], x + 82, 178, 28, i == v.item_cat ? kInk : kInkDim, Align::Left, tw - 96);
    }
    auto items = ItemsOf(s, v.item_cat);
    const int top = kItemListY;
    for (int r = 0; r < kItemRows; ++r) {
        const int idx = v.item_scroll + r;
        if (idx >= int(items.size())) break;
        const int y = top + r * kItemRowH;
        const bool sel = idx == v.item_sel;
        if (sel) {
            if (const Image* hl = a.image(Ui("Menu/T_CommonListSelect"))) c.Blit(*hl, 30, y, kW - 60, kItemRowH - 6);
            else c.Fill(30, y, kW - 60, kItemRowH - 6, C(0x605EC4E6));
        } else {
            c.Fill(30, y, kW - 60, kItemRowH - 6, r % 2 ? C(0x60142632) : C(0x80142632));
        }
        const int id = items[size_t(idx)].id;
        if (VisionFace(a, id)) VisionBadge(c, a, id, 46, y + 8, 62);
        else if (const Image* ic = a.image(ItemIcon(id))) c.Blit(*ic, 46, y + 8, 62, 62);
        // Who has it equipped: their faces, as the game marks equipped gear
        std::vector<int> users;
        for (auto& u : s.units) {
            bool on = u.weapon == id || u.armor == id || u.vision == id;
            for (int x : u.accessories) on = on || x == id;
            if (on && u.join == 0) users.push_back(u.id);
        }
        const float qty_w = a.bold->Measure(std::to_string(items[size_t(idx)].num), 38) + a.regular->Measure("x", 28) + 10;
        int fx = int(1190 - qty_w - 16 - 52 * int(users.size()));
        for (int uid : users) {
            if (const Image* face = a.image(Ui("Icon/Unit_Face_Timeline/ICON_UnitFace_unit" + std::to_string(uid)))) {
                c.Disc(float(fx + 23), float(y + 39), 25, C(0xC0101E28));
                c.BlitFit(*face, fx, y + 16, 46, 46);
            }
            fx += 52;
        }
        const int name_w = int(1190 - qty_w - 24 - 52 * int(users.size())) - 128;
        c.Text(*a.bold, ItemName(a, id), 128, y + 52, 36, kInk, Align::Left, float(std::max(200, name_w)), kShadow);
        c.Text(*a.regular, "x", 1190 - qty_w + 4, y + 52, 28, kInkDim);
        c.Text(*a.bold, std::to_string(items[size_t(idx)].num), 1190, y + 54, 38, kInk, Align::Right);
    }
    if (items.empty()) c.Text(*a.regular, "Nothing here yet.", kW / 2, 480, 34, kInkDim, Align::Center);
    // scroll indicator
    if (int(items.size()) > kItemRows) {
        const int track = kItemRows * kItemRowH;
        const int th = std::max(40, track * kItemRows / int(items.size()));
        const int ty = top + (track - th) * v.item_scroll / std::max(1, int(items.size()) - kItemRows);
        c.Fill(kW - 18, top, 6, track, C(0x40FFFFFF));
        c.Fill(kW - 18, ty, 6, th, kLine);
    }
    // description
    const int dy = top + kItemRows * kItemRowH + 6, dh = kTabBarY - dy - 14;
    Panel(c, a, 30, dy, kW - 60, dh);
    c.SetClip(30, dy, kW - 60, dh);  // nothing spills onto the tab bar
    if (v.item_sel >= 0 && v.item_sel < int(items.size())) {
        const auto lines = Wrap(*a.regular, ItemDesc(a, items[size_t(v.item_sel)].id), 28, kW - 104);
        const int max_lines = std::max(1, (dh - 20) / 36);
        for (int i = 0; i < int(lines.size()) && i < max_lines; ++i) {
            const bool last = i == max_lines - 1 && int(lines.size()) > max_lines;
            c.Text(*a.regular, last ? lines[size_t(i)] + " ..." : lines[size_t(i)], 52, dy + 42 + 36 * i, 28, kInk, Align::Left, kW - 104);
        }
    } else {
        c.Text(*a.regular, "Tap an item to read its description. Swipe to scroll.", 52, dy + 52, 26, kInkFaint, Align::Left, kW - 104);
    }
    c.ResetClip();
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
constexpr int kFieldY = kHeaderH + 10, kWinY = 560, kWinB = kTabBarY - 10;
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
    static const char* kTitles[kTabCount] = {"Map", "Party", "Items", "Battle"};
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
    default: Battle(c, s, a); break;
    }
    TabBar(c, a, v.tab, v.map_ok);
}

} // namespace ffr
