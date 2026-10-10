// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Class sprites, job names and HP-state colours for Dragon Quest III HD-2D Remake 1.1.0.0.
// Chain and conventions: dq3_sprites.h.

#include "dq3_sprites.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

#include "dq3_cooked.h"
#include "dq3_font.h"

namespace dq3 {
namespace {

#include "dq3_sprite_pins.inc"

using cooked::Le32;
float LeF(const u8* p) {
    const u32 v = Le32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

constexpr std::string_view JobIds[10] = {
    "Txt_Status_Job_Hero",     "Txt_Status_Job_Warrior",  "Txt_Status_Job_MartialArtist",
    "Txt_Status_Job_Mage",     "Txt_Status_Job_Priest",   "Txt_Status_Job_Merchant",
    "Txt_Status_Job_Gadabout", "Txt_Status_Job_Thief",    "Txt_Status_Job_Sage",
    "Txt_Status_Job_MonsterTamer"};
constexpr std::string_view HeroRoto = "Txt_Status_Job_Hero_Roto";
constexpr std::string_view NounPrefix = "TEXT_NOUN_ENGLISH_";

using namespace cooked;

std::mutex text_mutex;
std::shared_ptr<const GameText> text_cache;

std::mutex frame_mutex;
std::vector<std::pair<u32, std::shared_ptr<const Image>>> frame_cache; // LRU, most recent last
constexpr std::size_t FrameCacheMax = 16;

} // namespace

const PakMember& NounAssetMember() {
    return NounAsset;
}
const PakMember& NounExpMember() {
    return NounExp;
}

std::span<const SpritePin> SpritePins() {
    return SpritePinsTable;
}
std::span<const LooksRow> LooksRows() {
    return LooksRowsTable;
}

std::optional<u32> FindLooks(std::string_view looks_id) {
    const auto rows = LooksRows();
    const auto it = std::lower_bound(rows.begin(), rows.end(), looks_id,
                                     [](const LooksRow& r, std::string_view k) { return r.id < k; });
    if (it == rows.end() || it->id != looks_id)
        return std::nullopt;
    return it->sprite;
}

std::optional<std::array<u32, 4>> SpriteRect(std::span<const u8> sprite, const SpritePin& pin,
                                             std::string* why) {
    const auto fail = [why](const char* r) -> std::optional<std::array<u32, 4>> {
        if (why)
            *why = r;
        return std::nullopt;
    };
    if (pin.uv_at + 8 > sprite.size() || pin.dim_at + 8 > sprite.size())
        return fail("sprite member too short");
    const float v[4] = {LeF(sprite.data() + pin.uv_at), LeF(sprite.data() + pin.uv_at + 4),
                        LeF(sprite.data() + pin.dim_at), LeF(sprite.data() + pin.dim_at + 4)};
    std::array<u32, 4> r{};
    for (int i = 0; i < 4; ++i) {
        if (!(v[i] >= 0.0f && v[i] <= 4096.0f) || v[i] != std::floor(v[i]))
            return fail("sprite rect not integral");
        r[static_cast<std::size_t>(i)] = static_cast<u32>(v[i]);
    }
    if (r[2] == 0 || r[3] == 0 || r[0] + r[2] > pin.tex_w || r[1] + r[3] > pin.tex_h)
        return fail("sprite rect outside the atlas");
    if (r[0] != pin.rect[0] || r[1] != pin.rect[1] || r[2] != pin.rect[2] || r[3] != pin.rect[3])
        return fail("sprite rect differs from the pin");
    return r;
}

Image SpriteCanvas(const Image& frame) {
    Image out{SpriteBoxW, SpriteBoxH,
              std::vector<u8>(std::size_t{SpriteBoxW} * SpriteBoxH * 4, 0)};
    const int w = static_cast<int>(frame.width) * SpriteScale;
    const int h = std::min(static_cast<int>(frame.height) * SpriteScale, SpriteMaxH);
    // Pillow: int(x + 10 + (80 - w) / 2) and int(y + (188 - h) / 2) with x, y >= 0 -> floor.
    const int dx = SpriteBoxDx + static_cast<int>(std::floor((80 - w) / 2.0));
    const int dy = static_cast<int>(std::floor((SpriteBoxH - h) / 2.0));
    for (int y = 0; y < h; ++y) {
        const int ty = dy + y;
        if (ty < 0 || ty >= SpriteBoxH)
            continue;
        for (int x = 0; x < w; ++x) {
            const int tx = dx + x;
            if (tx < 0 || tx >= SpriteBoxW)
                continue;
            const u8* s = &frame.rgba[(std::size_t(y / SpriteScale) * frame.width +
                                       std::size_t(x / SpriteScale)) * 4];
            std::memcpy(&out.rgba[(std::size_t(ty) * SpriteBoxW + std::size_t(tx)) * 4], s, 4);
        }
    }
    return out;
}

std::shared_ptr<const Image> SpriteFrame(const RangeReader& read, u32 index, std::string* why) {
    {
        std::scoped_lock lock{frame_mutex};
        for (auto it = frame_cache.begin(); it != frame_cache.end(); ++it)
            if (it->first == index) {
                auto hit = std::move(*it);
                frame_cache.erase(it);
                frame_cache.push_back(hit);
                return hit.second;
            }
    }
    const auto pins = SpritePins();
    if (index >= pins.size()) {
        if (why)
            *why = "sprite index";
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
    // the same layout checks as a whole-atlas decode; only the frame is converted
    auto frame = DecodeTextureRect(
        *tex, TexturePin{pin.tex_w, pin.tex_h, pin.tex_w * pin.tex_h * 4, TextureFormat::Bgra8}, *rect, why);
    if (!frame)
        return nullptr;
    auto shared = std::make_shared<const Image>(std::move(*frame));
    std::scoped_lock lock{frame_mutex};
    frame_cache.emplace_back(index, shared);
    if (frame_cache.size() > FrameCacheMax)
        frame_cache.erase(frame_cache.begin());
    return shared;
}

std::optional<Image> ComposeSprite(const RangeReader& read, u32 index, std::string* why) {
    const auto frame = SpriteFrame(read, index, why);
    if (!frame)
        return std::nullopt;
    return SpriteCanvas(*frame);
}

std::optional<std::string_view> JobTextId(std::uint8_t vocation, bool roto) {
    if (vocation < 1 || vocation > 10)
        return std::nullopt;
    if (vocation == 1 && roto)
        return HeroRoto;
    return JobIds[vocation - 1];
}

std::optional<JobNames> ParseJobNames(std::span<const u8> uasset, std::span<const u8> uexp) {
    const auto pkg = ParseSummary(uasset);
    if (!pkg || pkg->export_offset + pkg->export_size > uexp.size())
        return std::nullopt;
    Cursor c{uexp.first(static_cast<std::size_t>(pkg->export_offset + pkg->export_size)),
             static_cast<std::size_t>(pkg->export_offset)};
    // UDataTable's own properties, then a 4-byte guid flag, then the rows.
    while (const auto t = NextTag(c, *pkg))
        c.Skip(static_cast<std::size_t>(t->size));
    if (!c.ok)
        return std::nullopt;
    c.Skip(4);
    const s32 rows = c.I32();
    if (!c.ok || rows <= 0 || rows > 100000)
        return std::nullopt;
    JobNames out;
    const std::string prefix = std::string{NounPrefix} + "Status_Job_";
    for (s32 k = 0; k < rows; ++k) {
        const s32 key = c.I32();
        const s32 num = c.I32();
        if (!c.ok || key < 0 || static_cast<std::size_t>(key) >= pkg->names.size() || num < 0)
            return std::nullopt;
        std::string row = pkg->names[static_cast<std::size_t>(key)];
        if (num)
            row += "_" + std::to_string(num - 1);
        const bool wanted = row.starts_with(prefix);
        while (const auto t = NextTag(c, *pkg)) {
            const std::size_t end = c.o + static_cast<std::size_t>(t->size);
            if (wanted && t->name == "ListNoun" && t->type == "StrProperty") {
                const auto s = c.FString();
                if (!s || c.o != end)
                    return std::nullopt;
                out["Txt_" + row.substr(NounPrefix.size())] = *s;
            }
            c.o = end;
        }
        if (!c.ok)
            return std::nullopt;
    }
    for (const auto id : JobIds)
        if (!out.contains(id))
            return std::nullopt;
    if (!out.contains(HeroRoto))
        return std::nullopt;
    return out;
}

u8 LinearToSrgb8(float linear) {
    const double c = std::clamp(static_cast<double>(linear), 0.0, 1.0);
    const double s = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    return static_cast<u8>(std::lround(s * 255.0));
}

std::optional<TextColours> ParseTextColours(std::span<const u8> uexp) {
    u32 argb[3]{};
    for (int i = 0; i < 3; ++i) {
        const u32 at = TextColorAt[i];
        if (at + 16 > uexp.size())
            return std::nullopt;
        float v[4];
        for (int k = 0; k < 4; ++k) {
            v[k] = LeF(uexp.data() + at + 4 * k);
            if (!(v[k] >= 0.0f && v[k] <= 1.0f))
                return std::nullopt;
        }
        if (v[3] != 1.0f)
            return std::nullopt;
        argb[i] = 0xFF000000u | u32{LinearToSrgb8(v[0])} << 16 | u32{LinearToSrgb8(v[1])} << 8 |
                  LinearToSrgb8(v[2]);
    }
    return TextColours{argb[0], argb[1], argb[2]};
}

std::shared_ptr<const GameText> LoadGameText(const RangeReader& read, std::string* why) {
    {
        std::scoped_lock lock{text_mutex};
        if (text_cache)
            return text_cache;
    }
    std::string reason;
    const auto asset = ReadMember(read, NounAsset, &reason);
    const auto exp = asset ? ReadMember(read, NounExp, &reason) : std::nullopt;
    const auto mat = exp ? ReadMember(read, TextColorMaterial, &reason) : std::nullopt;
    if (!mat) {
        if (why)
            *why = reason;
        return nullptr;
    }
    auto jobs = ParseJobNames(*asset, *exp);
    auto colours = ParseTextColours(*mat);
    if (!jobs || !colours) {
        if (why)
            *why = jobs ? "text colour parse" : "job name table parse";
        return nullptr;
    }
    auto built = std::make_shared<const GameText>(GameText{std::move(*jobs), *colours});
    std::scoped_lock lock{text_mutex};
    if (!text_cache)
        text_cache = std::move(built);
    return text_cache;
}

std::shared_ptr<const GameText> CachedGameText() {
    std::scoped_lock lock{text_mutex};
    return text_cache;
}

} // namespace dq3
