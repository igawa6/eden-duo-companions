// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Battle page art for Dragon Quest III HD-2D Remake 1.1.0.0. Keys and chain: dq3_battle_art.h.

#include "dq3_battle_art.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "dq3_art.h"

namespace dq3 {
namespace {

#include "dq3_battle_pins.inc"

using art_util::Blank, art_util::InRound, art_util::ParseInt, art_util::RoundShape, art_util::Split;

constexpr u8 Gold[4] = {236, 210, 146, 255}; // revision 2 GOLD
constexpr u8 CellLine[4] = {96, 90, 74, 255};
constexpr u8 Separator[4] = {110, 106, 92, 255};
constexpr u8 GridLine[4] = {150, 118, 76, 255};

enum class Ui { TravelInfoBg, Select, MonsterShadow, SpellIcon, Count };

std::mutex art_mutex;
std::shared_ptr<const Image> ui_cache[static_cast<std::size_t>(Ui::Count)];
std::vector<std::pair<u32, std::shared_ptr<const Image>>> frame_cache; // small LRU

std::shared_ptr<const Image> UiTexture(const RangeReader& read, Ui id, std::string* why) {
    const auto i = static_cast<std::size_t>(id);
    {
        std::scoped_lock lock{art_mutex};
        if (ui_cache[i])
            return ui_cache[i];
    }
    static const PakMember* members[] = {&UiTravelInfoBgMember, &UiSelectMember,
                                         &UiMonsterShadowMember, &UiSpellIconMember};
    static const TexturePin layouts[] = {UiTravelInfoBgLayout, UiSelectLayout,
                                         UiMonsterShadowLayout, UiSpellIconLayout};
    const auto bytes = ReadMember(read, *members[i], why);
    if (!bytes)
        return nullptr;
    auto img = DecodeTexture(*bytes, layouts[i], why);
    if (!img)
        return nullptr;
    auto shared = std::make_shared<const Image>(std::move(*img));
    std::scoped_lock lock{art_mutex};
    ui_cache[i] = shared;
    return shared;
}

/// The idle frame of monster sprite `index` (straight RGBA, atlas crop).
std::shared_ptr<const Image> MonsterFrame(const RangeReader& read, u32 index, std::string* why) {
    {
        std::scoped_lock lock{art_mutex};
        for (const auto& [k, v] : frame_cache)
            if (k == index)
                return v;
    }
    const auto pins = MonsterSpritePins();
    if (index >= pins.size()) {
        if (why)
            *why = "monster sprite index";
        return nullptr;
    }
    const SpritePin& pin = pins[index];
    std::string reason;
    const auto sprite = ReadMember(read, pin.sprite, &reason);
    if (!sprite) {
        if (why)
            *why = std::string{pin.sprite.path} + ": " + reason;
        return nullptr;
    }
    const auto rect = SpriteRect(*sprite, pin, why);
    if (!rect)
        return nullptr;
    const auto tex = ReadMember(read, pin.texture, &reason);
    if (!tex) {
        if (why)
            *why = std::string{pin.texture.path} + ": " + reason;
        return nullptr;
    }
    auto frame = DecodeTextureRect(
        *tex, TexturePin{pin.tex_w, pin.tex_h, pin.tex_w * pin.tex_h * 4, TextureFormat::Bgra8}, *rect, why);
    if (!frame)
        return nullptr;
    auto shared = std::make_shared<const Image>(std::move(*frame));
    std::scoped_lock lock{art_mutex};
    frame_cache.emplace_back(index, shared);
    if (frame_cache.size() > 24)
        frame_cache.erase(frame_cache.begin());
    return shared;
}

/// The design's monster sprite at its draw size: integer nearest scale, or a LANCZOS reduction
/// when the frame is larger than the box.
Image FitMonster(const Image& frame, u32 max_w, u32 max_h, int max_k = 1 << 20) {
    const int scale = std::min(max_k, MonsterScale(frame.width, frame.height, max_w, max_h));
    if (scale == 0) {
        const double f = std::min(static_cast<double>(max_w) / frame.width,
                                  static_cast<double>(max_h) / frame.height);
        const u32 w = std::max<u32>(1, static_cast<u32>(frame.width * f));
        const u32 h = std::max<u32>(1, static_cast<u32>(frame.height * f));
        return Resize(frame, 0, 0, frame.width, frame.height, w, h);
    }
    const u32 s = static_cast<u32>(scale);
    Image out{frame.width * s, frame.height * s, {}};
    out.rgba.resize(std::size_t{out.width} * out.height * 4);
    for (u32 y = 0; y < out.height; ++y)
        for (u32 x = 0; x < out.width; ++x)
            std::memcpy(&out.rgba[(std::size_t{y} * out.width + x) * 4],
                        &frame.rgba[(std::size_t{y / s} * frame.width + x / s) * 4], 4);
    return out;
}

/// Pillow convert("L") then RGB, alpha x 0.35 (truncated), as render.py grey().
void Grey(Image& img) {
    for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
        const unsigned l = (img.rgba[i] * 299u + img.rgba[i + 1] * 587u + img.rgba[i + 2] * 114u) / 1000u;
        img.rgba[i] = img.rgba[i + 1] = img.rgba[i + 2] = static_cast<u8>(l);
        img.rgba[i + 3] = static_cast<u8>(img.rgba[i + 3] * 0.35);
    }
}

/// render.py cell_glow(): a 5 px rounded ring (radius 9) blurred (Gaussian, sigma 4), x 2.2, in
/// (255, 214, 110), then the 3 px gold box; the glow spreads `pad` px around the box (12, list rows 8).
void Glow(Image& dst, int x, int y, int w, int h) {
    const int W = static_cast<int>(dst.width), H = static_cast<int>(dst.height);
    std::vector<float> m(static_cast<std::size_t>(W) * H, 0.0f), t(m.size(), 0.0f);
    for (int yy = 0; yy < H; ++yy)
        for (int xx = 0; xx < W; ++xx) {
            const double px = xx + 0.5, py = yy + 0.5;
            if (InRound(px, py, x, y, x + w + 1, y + h + 1, 9) &&
                !InRound(px, py, x + 5, y + 5, x + w + 1 - 5, y + h + 1 - 5, 4))
                m[static_cast<std::size_t>(yy) * W + xx] = 255.0f;
        }
    constexpr double Sigma = 4.0;
    const int rad = 12;
    std::vector<double> k(2 * rad + 1);
    double sum = 0;
    for (int i = -rad; i <= rad; ++i)
        sum += k[static_cast<std::size_t>(i + rad)] = std::exp(-(i * i) / (2 * Sigma * Sigma));
    for (auto& v : k)
        v /= sum;
    for (int yy = 0; yy < H; ++yy)
        for (int xx = 0; xx < W; ++xx) {
            double s = 0;
            for (int i = -rad; i <= rad; ++i) {
                const int sx = xx + i;
                if (sx >= 0 && sx < W)
                    s += k[static_cast<std::size_t>(i + rad)] * m[static_cast<std::size_t>(yy) * W + sx];
            }
            t[static_cast<std::size_t>(yy) * W + xx] = static_cast<float>(s);
        }
    Image glow{dst.width, dst.height, std::vector<u8>(dst.rgba.size(), 0)};
    for (int yy = 0; yy < H; ++yy)
        for (int xx = 0; xx < W; ++xx) {
            double s = 0;
            for (int i = -rad; i <= rad; ++i) {
                const int sy = yy + i;
                if (sy >= 0 && sy < H)
                    s += k[static_cast<std::size_t>(i + rad)] * t[static_cast<std::size_t>(sy) * W + xx];
            }
            const int a = std::min(255, static_cast<int>(static_cast<int>(s) * 2.2));
            u8* p = &glow.rgba[(static_cast<std::size_t>(yy) * W + xx) * 4];
            p[0] = 255;
            p[1] = 214;
            p[2] = 110;
            p[3] = static_cast<u8>(a);
        }
    Over(dst, glow, 0, 0);
    RoundShape(dst, x, y, x + w, y + h, 9, 3, Gold);
}

std::optional<Image> Compose(const RangeReader& read, std::string_view key, std::string* why) {
    const auto parts = Split(key);
    const auto size = parts.size() > 1 ? ParseSize(parts[1]) : std::nullopt;
    const std::string_view kind = parts[0];
    if (kind == "light" && parts.size() == 2 && size) {
        const auto tex = UiTexture(read, Ui::TravelInfoBg, why);
        if (!tex || size->first <= 60 || size->second <= 60)
            return std::nullopt;
        return LightScrap(*tex, size->first, size->second);
    }
    if (kind == "rowglow" && parts.size() == 2 && size) {
        // a Bag list row's selection: the cell glow with an 8 px spread around a w x h box
        constexpr int pad = 8;
        Image out = Blank(size->first + 2 * pad, size->second + 2 * pad);
        Glow(out, pad, pad, static_cast<int>(size->first), static_cast<int>(size->second));
        return out;
    }
    if (kind == "ecell" && parts.size() == 6) {
        // ecell/<sprite|-1>/<w>x<h>/<sprite box h>/<mode>/<flags>: the cell outline (or the glow, flag s)
        // GLOW px larger on every side and the monster sprite (flag g: grey). Mode 1 (battle cells with
        // names, render.py enemy_box): fitted into (w - 30) x box h at scale <= 2, centred, bottom at 6 + box h.
        // Mode 0 (records, bare battle cells): fitted into (w - 20) x box h at scale <= 3, centred.
        int s = 0, sp_h = 0, mode = 0;
        const auto box = ParseSize(parts[2]);
        if (!ParseInt(parts[1], s) || !box || !ParseInt(parts[3], sp_h) || !ParseInt(parts[4], mode) || sp_h <= 0 ||
            mode < 0 || mode > 1 || box->first < 40 || box->second < 20)
            return std::nullopt;
        const bool sel = parts[5].find('s') != std::string_view::npos;
        const bool grey = parts[5].find('g') != std::string_view::npos;
        const int w = static_cast<int>(box->first), h = static_cast<int>(box->second), pad = CellGlow;
        Image out;
        out = Blank(box->first + 2 * pad, box->second + 2 * pad);
        if (sel)
            Glow(out, pad, pad, w, h);
        else
            RoundShape(out, pad, pad, pad + w, pad + h, 8, 1, CellLine);
        if (s >= 0) {
            const auto frame = MonsterFrame(read, static_cast<u32>(s), why);
            if (!frame)
                return std::nullopt;
            const u32 mw = static_cast<u32>(std::max(1, w - (mode ? 30 : 20)));
            Image sp = FitMonster(*frame, mw, static_cast<u32>(sp_h), mode ? 2 : 3);
            if (grey)
                Grey(sp);
            const int sx = static_cast<int>(std::floor((w - static_cast<int>(sp.width)) / 2.0));
            const int sy = mode ? 6 + sp_h - static_cast<int>(sp.height)
                                : static_cast<int>(std::floor((h - static_cast<int>(sp.height)) / 2.0));
            Over(out, sp, pad + sx, pad + sy);
        }
        return out;
    }
    if (kind == "mon" && (parts.size() == 3 || parts.size() == 4)) {
        // mon/<sprite>/<w>x<h>[/<max scale>]: the sprite fitted and centred in the box
        int s = 0, max_k = 1 << 20;
        const auto box = ParseSize(parts[2]);
        if (!ParseInt(parts[1], s) || s < 0 || !box || (parts.size() == 4 && (!ParseInt(parts[3], max_k) || max_k < 1)))
            return std::nullopt;
        const auto frame = MonsterFrame(read, static_cast<u32>(s), why);
        if (!frame)
            return std::nullopt;
        const Image sp = FitMonster(*frame, box->first, box->second, max_k);
        Image out;
        out = Blank(box->first, box->second);
        Over(out, sp, static_cast<int>(std::floor((box->first - sp.width) / 2.0)),
             static_cast<int>(std::floor((box->second - sp.height) / 2.0)));
        return out;
    }
    if (kind == "tabs" && parts.size() == 4 && size) {
        // tabs/<w>x<h>/<sel>/<chevron x>: render.py command_tabs(['Tactical', 'Follow'], icons=False): window
        // 0.88, the gold ring (sx + 6, 8, sx + slot - 6, h - 8) on the selected half, the separator
        // (14 .. h - 14) and the 34 px chevron
        int sel = 0, chev = 0;
        if (!ParseInt(parts[2], sel) || !ParseInt(parts[3], chev) || sel < 0 || sel > 1)
            return std::nullopt;
        const auto win = UiTexture(read, Ui::Select, why); // chevron
        Image out;
        out = Blank(size->first, size->second);
        std::string reason;
        const auto wb = ReadMember(read, Member(MemberId::WindowBase), &reason);
        const auto wt = wb ? DecodeTexture(*wb, *TextureLayout(MemberId::WindowBase), &reason) : std::nullopt;
        if (!wt || !win) {
            if (why && !wt)
                *why = reason;
            return std::nullopt;
        }
        DrawWindow(*wt, out, 0, 0, out.width, out.height, 0.88);
        const int w = static_cast<int>(out.width), h = static_cast<int>(out.height);
        const double slot = w / 2.0;
        const double sx = sel * slot;
        for (int y = 14; y <= h - 14; ++y)
            std::memcpy(&out.rgba[(static_cast<std::size_t>(y) * out.width + static_cast<u32>(slot)) * 4],
                        Separator, 4);
        RoundShape(out, static_cast<int>(sx + 6), 8, static_cast<int>(sx + slot - 6), h - 8, 8, 3, Gold);
        const u32 cs = static_cast<u32>(std::lround(h * 34.0 / 88.0));
        const Image c = Resize(*win, 0, 0, win->width, win->height, cs, cs);
        Over(out, c, chev, static_cast<int>((h - static_cast<int>(cs)) / 2.0));
        return out;
    }
    if (kind == "outline" && parts.size() == 2 && size) {
        Image out;
        out = Blank(size->first, size->second);
        // render.py outline(): rounded_rectangle((2, 2, w - 2, h - 2), 10, width 3)
        RoundShape(out, 2, 2, static_cast<int>(out.width) - 2, static_cast<int>(out.height) - 2, 10, 3, Gold);
        return out;
    }
    if ((kind == "chev" || kind == "down" || kind == "shadowq") && parts.size() == 2) {
        int px = 0;
        if (!ParseInt(parts[1], px) || px <= 0 || px > 256)
            return std::nullopt;
        const auto tex = UiTexture(read, kind == "shadowq" ? Ui::MonsterShadow : Ui::Select, why);
        if (!tex)
            return std::nullopt;
        const u32 sw = kind == "shadowq" ? tex->width / 2 : tex->width;
        Image img = Resize(*tex, 0, 0, sw, tex->height, static_cast<u32>(px), static_cast<u32>(px));
        if (kind != "down")
            return img;
        Image rot{img.height, img.width, std::vector<u8>(img.rgba.size())};
        for (u32 y = 0; y < img.height; ++y) // clockwise: (x, y) -> (h - 1 - y, x)
            for (u32 x = 0; x < img.width; ++x)
                std::memcpy(&rot.rgba[(std::size_t{x} * rot.width + (img.height - 1 - y)) * 4],
                            &img.rgba[(std::size_t{y} * img.width + x) * 4], 4);
        return rot;
    }
    if (kind == "affgrid" && (parts.size() == 3 || parts.size() == 4) && size) {
        int cols = 0, rows = 1;
        if (!ParseInt(parts[2], cols) || cols < 1 || cols > 12 ||
            (parts.size() == 4 && (!ParseInt(parts[3], rows) || rows < 1 || rows > 8)))
            return std::nullopt;
        Image out;
        out = Blank(size->first + 1, size->second + 1);
        const int w = static_cast<int>(size->first), h = static_cast<int>(size->second);
        const auto px = [&](int x, int y) {
            std::memcpy(&out.rgba[(static_cast<std::size_t>(y) * out.width + static_cast<u32>(x)) * 4], GridLine, 4);
        };
        for (int x = 0; x <= w; ++x) {
            px(x, 0);
            px(x, h);
            for (int k = 1; k < rows; ++k)
                px(x, static_cast<int>(k * static_cast<double>(h) / rows));
        }
        for (int y = 0; y <= h; ++y) {
            px(0, y);
            px(w, y);
            for (int k = 1; k < cols; ++k)
                px(static_cast<int>(k * static_cast<double>(w) / cols), y);
        }
        return out;
    }
    if (kind == "pcsprite" && parts.size() == 4) {
        int n = 0, box_h = 0;
        if (!ParseInt(parts[1], n) || n < 0 || !ParseInt(parts[2], box_h) || box_h <= 0 || box_h > 400 ||
            (parts[3] != "b" && parts[3] != "c"))
            return std::nullopt;
        const auto frame = SpriteFrame(read, static_cast<u32>(n), why);
        if (!frame)
            return std::nullopt;
        Image out;
        out = Blank(PcSpriteW, static_cast<u32>(box_h));
        const int w = static_cast<int>(frame->width) * 2;
        const int h = std::min(static_cast<int>(frame->height) * 2, box_h);
        const int dx = static_cast<int>(std::floor((PcSpriteW - w) / 2.0));
        const int dy = parts[3] == "b" ? box_h - h : static_cast<int>(std::floor((box_h - h) / 2.0));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int tx = dx + x;
                if (tx < 0 || tx >= static_cast<int>(PcSpriteW))
                    continue;
                std::memcpy(&out.rgba[(static_cast<std::size_t>(dy + y) * PcSpriteW + static_cast<u32>(tx)) * 4],
                            &frame->rgba[(std::size_t(y / 2) * frame->width + std::size_t(x / 2)) * 4], 4);
            }
        return out;
    }
    if (kind == "spell" && (parts.size() == 3 || (parts.size() == 4 && parts[3] == "g"))) {
        int cell = 0, px = 0;
        if (!ParseInt(parts[1], cell) || !ParseInt(parts[2], px) || cell < 0 || cell >= 8 || px <= 0 || px > 256)
            return std::nullopt;
        const auto tex = UiTexture(read, Ui::SpellIcon, why);
        if (!tex)
            return std::nullopt;
        const u32 cw = tex->width / 4, ch = tex->height / 2;
        Image out = Resize(*tex, static_cast<u32>(cell % 4) * cw, static_cast<u32>(cell / 4) * ch, cw, ch,
                           static_cast<u32>(px), static_cast<u32>(px));
        if (parts.size() == 4) // greyed (battle-only spells in the field list)
            Grey(out);
        return out;
    }
    return std::nullopt;
}

} // namespace

int MonsterScale(u32 w, u32 h, u32 max_w, u32 max_h) {
    if (w == 0 || h == 0)
        return 0;
    if (w > max_w || h > max_h)
        return 0;
    return static_cast<int>(std::max<u32>(1, std::min(max_h / h, max_w / w)));
}

std::span<const SpritePin> MonsterSpritePins() {
    return MonsterSpritePinsTable;
}

std::optional<u32> FindMonsterSprite(std::string_view unit_master) {
    const std::span<const LooksRow> rows = MonsterRowsTable;
    const auto it = std::lower_bound(rows.begin(), rows.end(), unit_master,
                                     [](const LooksRow& r, std::string_view k) { return r.id < k; });
    if (it == rows.end() || it->id != unit_master)
        return std::nullopt;
    return it->sprite;
}

const PakMember& BattleTableMember(BattleTable id) {
    switch (id) {
    case BattleTable::MonsterAsset:
        return GopMonsterUasset;
    case BattleTable::MonsterExp:
        return GopMonsterUexp;
    case BattleTable::ResistAsset:
        return GopBattle_ResistUasset;
    case BattleTable::ResistExp:
        return GopBattle_ResistUexp;
    case BattleTable::MagicAsset:
        return GopMagicUasset;
    case BattleTable::MagicExp:
        return GopMagicUexp;
    case BattleTable::ItemAsset:
        return GopItemUasset;
    case BattleTable::ItemExp:
        return GopItemUexp;
    case BattleTable::ActionAsset:
        return GopBattle_Monster_ActionUasset;
    case BattleTable::ActionExp:
        return GopBattle_Monster_ActionUexp;
    case BattleTable::ActionListAsset:
        return GopBattle_Monster_ActionListUasset;
    case BattleTable::ActionListExp:
        return GopBattle_Monster_ActionListUexp;
    default:
        return GopItemUexp;
    }
}

bool IsBattleArtKey(std::string_view key) {
    for (const std::string_view p : {"light/", "rowglow/", "ecell/", "mon/", "pcsprite/", "tabs/", "outline/", "chev/",
                                     "down/", "shadowq/", "affgrid/", "spell/"})
        if (key.starts_with(p))
            return true;
    return false;
}

std::optional<Image> ComposeBattleArt(const RangeReader& read, std::string_view key, std::string* why) {
    if (!IsBattleArtKey(key))
        return std::nullopt;
    try {
        return Compose(read, key, why);
    } catch (...) {
        if (why)
            *why = "battle art exception";
        return std::nullopt;
    }
}

} // namespace dq3
