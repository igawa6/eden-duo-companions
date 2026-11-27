// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xcx_avatar.h"
#include "xcx_bdat.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace xcx {
namespace {
using bdat::At;
using bdat::Table;
// common.bdat tables (file indices): ITM_AvatarHairList and CLR_List.
constexpr float LiftGain = 1.26f, LiftAdd = 50.f;
constexpr std::array<float, 3> HairGains{1.9f, 1.65f, 1.6f}; // per channel, fitted on the Zanemato card
constexpr unsigned HairTable = 420, ColourTable = 148;
constexpr std::uint32_t HashResourceId = 0xc8288c8f, HashRed = 0xd6d7782b, HashGreen = 0xade175dc,
                        HashBlue = 0xb5e8ebfd, HashAlpha = 0x7fc7e87d;
std::array<float, 4> Colour(const Table &t, int r, int g, int bl, int a, unsigned id) {
    if (id == 0 || id < t.base || id - t.base >= t.rows) // the game's own neutral fallback
        return {0.5f, 0.5f, 0.5f, 1.f};
    const auto row = t.Row(id - t.base);
    return {t.b[row + t.offset[r]] / 255.f, t.b[row + t.offset[g]] / 255.f,
            t.b[row + t.offset[bl]] / 255.f, t.b[row + t.offset[a]] * (1.f / 128.f)};
}
} // namespace
std::optional<AvatarPortraitSpec> ResolveAvatar(Archive &archive, const EdenDsmodHostApi &host,
                                                const AvatarLook &look) {
    if (!look.valid)
        return std::nullopt;
    const auto bytes = archive.Read(host, "/bdat/common.bdat");
    const std::span<const std::uint8_t> b = bytes;
    Table hair, colour;
    if (!hair.Open(b, HairTable) || !colour.Open(b, ColourTable))
        return std::nullopt;
    const int resource = hair.Column(HashResourceId, 2);
    const int r = colour.Column(HashRed, 1), g = colour.Column(HashGreen, 1),
              bl = colour.Column(HashBlue, 1), a = colour.Column(HashAlpha, 1);
    if (resource < 0 || r < 0 || g < 0 || bl < 0 || a < 0)
        return std::nullopt;
    // cell = (row id whose ResourceID equals HairBaseID) - 1; the game falls back to cell 0.
    AvatarPortraitSpec spec;
    for (unsigned row = 0; row < hair.rows; ++row)
        if (At<std::uint16_t>(b, hair.Row(row) + hair.offset[resource]) == look.hair) {
            spec.cell = int(hair.base + row) - 1;
            break;
        }
    if (spec.cell < 0 || spec.cell >= 16 * 9)
        spec.cell = 0;
    spec.tint = Colour(colour, r, g, bl, a, look.color5);
    return spec;
}
Image ComposeAvatar(const Image &base, const Image &mask, const AvatarPortraitSpec &spec) {
    if (base.w != 1792 || base.h != 1008 || mask.w != base.w || mask.h != base.h || spec.cell < 0 ||
        spec.cell >= 16 * 9)
        return {};
    Image out(112, 112);
    const int sx = spec.cell % 16 * 112, sy = spec.cell / 16 * 112;
    // The palette colours are 2x-overbright multipliers (neutral grey 0.5 -> 1.0).
    std::array<float, 3> k;
    for (int c = 0; c < 3; ++c)
        k[c] = std::clamp(2.f * spec.tint[c] * spec.tint[3], 0.f, 2.f);
    for (int y = 0; y < 112; ++y)
        for (int x = 0; x < 112; ++x) {
            const auto at = (std::size_t(sy + y) * base.w + sx + x) * 4;
            const auto *s = base.rgba.data() + at;
            const float w = mask.rgba[at] / 255.f;                      // red channel: the hair
            const float lift = std::max(w, mask.rgba[at + 1] / 255.f); // hair or skin: lit
            auto *d = out.rgba.data() + (std::size_t(y) * 112 + x) * 4;
            for (int c = 0; c < 3; ++c) {
                // Empirical match of the native card's lighting (fitted on the Zanemato card): skin
                // is lifted affinely, hair is the tinted base with a gain; clothes stay as drawn.
                const float skin =
                    std::clamp(s[c] + ((s[c] * LiftGain + LiftAdd) - s[c]) * lift, 0.f, 255.f);
                const float hair = std::clamp(s[c] * k[c] * HairGains[c], 0.f, 255.f);
                d[c] = std::uint8_t(std::lround(skin + (hair - skin) * w));
            }
            d[3] = s[3];
        }
    return out;
}
Image AvatarPortrait(Assets &assets, const EdenDsmodHostApi &host, const AvatarLook &look) {
    try {
        const auto spec = ResolveAvatar(assets.Files(), host, look);
        if (!spec)
            return {};
        return ComposeAvatar(assets.Ui(host, "icon_face_avatar", 0),
                             assets.Ui(host, "icon_face_avatar", 1), *spec);
    } catch (const std::exception &) {
        return {};
    }
}
} // namespace xcx
