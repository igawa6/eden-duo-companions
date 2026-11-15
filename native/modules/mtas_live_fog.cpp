// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mtas_live_fog.h"
#include "mtas_map.h"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace mtas {
void LiveFog::Configure(const EdenDsmodHostTextureApi* api) {
    if (!api || api->version != EDEN_DSMOD_TEXTURE_EXT_VERSION ||
        api->struct_size < sizeof(*api) || api->abi_hash != EDEN_DSMOD_TEXTURE_EXT_HASH ||
        !api->request || !api->poll || !api->cancel)
        return;
    api_ = *api;
}

void LiveFog::Close() {
    address_ = generation_ = 0;
    key_.clear();
    image_ = {};
    raw_.clear();
    trusted_.clear();
    tiles_.clear();
    gated_ = 0;
}

void LiveFog::Reset() {
    if (ticket_) api_.cancel(api_.userdata, ticket_);
    ticket_ = 0;
    info_ = {};
    next_ = {};
    explored_.clear();
    Close();
}

ce_unity::Image LiveFog::Decode(const std::uint8_t* rgba) {
    static const auto alpha = [] {
        std::array<std::uint8_t, 256> values{};
        for (int i = 0; i < 256; ++i) {
            const double s = i / 255.0;
            const double linear = s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
            // Round towards fog, never towards extra visibility.
            values[i] = static_cast<std::uint8_t>(std::ceil(255 * (1 - linear)));
        }
        return values;
    }();
    ce_unity::Image out{256, 256, std::vector<std::uint8_t>(256 * 256 * 4, 255)};
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x)
            out.rgba[(y * 256 + x) * 4 + 3] = alpha[rgba[((255 - y) * 256 + x) * 4]];
    return out;
}

std::vector<std::uint8_t> LiveFog::TileCrop(const std::vector<std::uint8_t>& reveal, int tile) {
    constexpr int N = FogSide, cell = FogSide / MapGrid;
    std::vector<std::uint8_t> out(std::size_t{TileMaskPx} * TileMaskPx);
    if (reveal.size() != std::size_t{N} * N || tile < 0 || tile >= MapTiles)
        return out;
    // The host stretches the mask over the tile with bilinear sampling, pixel i centred at
    // (i + 0.5) / TileMaskPx of the tile: sample the whole-map bilinear reveal at exactly that
    // point (in cell units, cell centres at + 0.5), so a tile's edge continues its neighbour's.
    const auto at = [](int origin, std::uint32_t i) {
        const double t = origin + (i + 0.5) * cell / TileMaskPx - 0.5;
        return std::clamp(t, 0.0, double(N - 1));
    };
    const int ox = (tile % MapGrid) * cell, oy = (tile / MapGrid) * cell;
    for (std::uint32_t j = 0; j < TileMaskPx; ++j) {
        const double ty = at(oy, j);
        const int y0 = static_cast<int>(ty), y1 = std::min(y0 + 1, N - 1);
        const double fy = ty - y0;
        for (std::uint32_t i = 0; i < TileMaskPx; ++i) {
            const double tx = at(ox, i);
            const int x0 = static_cast<int>(tx), x1 = std::min(x0 + 1, N - 1);
            const double fx = tx - x0;
            const double top = reveal[y0 * N + x0] * (1 - fx) + reveal[y0 * N + x1] * fx;
            const double bottom = reveal[y1 * N + x0] * (1 - fx) + reveal[y1 * N + x1] * fx;
            // Round down (never more visible than the full-map sample); the epsilon keeps a
            // uniformly revealed run at 255 despite floating-point error.
            out[j * TileMaskPx + i] =
                static_cast<std::uint8_t>(std::floor(top + (bottom - top) * fy + 1e-6));
        }
    }
    return out;
}

void LiveFog::SetExplored(std::vector<std::uint8_t> cells) {
    if (cells.size() != std::size_t{FogSide} * FogSide)
        cells.clear();
    if (cells == explored_)
        return;
    explored_ = std::move(cells);
    if (Ready())
        Compose();
}

void LiveFog::Compose() {
    constexpr std::size_t cells = std::size_t{FogSide} * FogSide;
    std::vector<std::uint8_t> gated(cells, 0);
    gated_ = 0;
    for (std::size_t i = 0; i < cells; ++i) {
        const bool explored = (explored_.size() == cells && explored_[i]) ||
                              (trusted_.size() == cells && trusted_[i]);
        gated[i] = explored ? raw_[i] : 0;
        gated_ += raw_[i] != 0 && !explored;
    }
    ce_unity::Image image{FogSide, FogSide, std::vector<std::uint8_t>(cells * 4, 255)};
    for (std::size_t i = 0; i < cells; ++i)
        image.rgba[i * 4 + 3] = static_cast<std::uint8_t>(255 - gated[i]);
    if (key_.empty() || image.rgba != image_.rgba) {
        image_ = std::move(image);
        key_ = "module:mtas:fog:" + std::to_string(epoch_) + "#" + std::to_string(++revision_);
    }
    tiles_.resize(MapTiles);
    for (int t = 0; t < MapTiles; ++t) {
        auto& tile = tiles_[t];
        // A tile's samples only reach the cells of the tile and one ring around it.
        constexpr int N = FogSide, cell = FogSide / MapGrid;
        const int x0 = std::max(0, (t % MapGrid) * cell - 1), x1 = std::min(N, (t % MapGrid + 1) * cell + 1);
        const int y0 = std::max(0, (t / MapGrid) * cell - 1), y1 = std::min(N, (t / MapGrid + 1) * cell + 1);
        int lo = 255, hi = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                lo = std::min<int>(lo, gated[y * N + x]);
                hi = std::max<int>(hi, gated[y * N + x]);
            }
        tile.state = hi == 0 ? TileState::Hidden : lo == 255 ? TileState::Full : TileState::Masked;
        if (tile.state != TileState::Masked) {
            tile.mask = {};
            tile.key.clear();
            continue;
        }
        const auto crop = TileCrop(gated, t);
        ce_unity::Image mask{TileMaskPx, TileMaskPx,
                             std::vector<std::uint8_t>(std::size_t{TileMaskPx} * TileMaskPx * 4, 255)};
        for (std::size_t i = 0; i < crop.size(); ++i)
            mask.rgba[i * 4 + 3] = crop[i];
        if (tile.key.empty() || mask.rgba != tile.mask.rgba) {
            tile.mask = std::move(mask);
            tile.key = "module:mtas:fog:" + std::to_string(epoch_) + ":" + std::to_string(t) + "#" +
                       std::to_string(++tile.revision);
        }
    }
}

int LiveFog::RevealAt(float u, float v) const {
    if (!Ready() || image_.rgba.size() != std::size_t{FogSide} * FogSide * 4 || !(u >= 0.0f) ||
        !(u < 1.0f) || !(v > 0.0f) || !(v <= 1.0f))
        return 0;
    const auto x = std::min(FogSide - 1, static_cast<int>(u * FogSide));
    const auto y = std::min(FogSide - 1, static_cast<int>((1.0f - v) * FogSide));
    return 255 - image_.rgba[(std::size_t(y) * FogSide + x) * 4 + 3];
}

LiveFog::TileState LiveFog::Tile(int tile) const {
    return Ready() && tile >= 0 && tile < static_cast<int>(tiles_.size()) ? tiles_[tile].state
                                                                          : TileState::Hidden;
}

const std::string& LiveFog::TileMaskKey(int tile) const {
    static const std::string none;
    return Tile(tile) == TileState::Masked ? tiles_[tile].key : none;
}

const ce_unity::Image& LiveFog::TileMask(int tile) const {
    static const ce_unity::Image none;
    return Tile(tile) == TileState::Masked ? tiles_[tile].mask : none;
}

const ce_unity::Image* LiveFog::Find(std::string_view key) const {
    if (!Ready() || key.empty())
        return nullptr;
    if (key == key_)
        return &image_;
    // module:mtas:fog:<epoch>:<tile>#<revision>
    const auto colon = key.rfind(':'), hash = key.rfind('#');
    if (colon == std::string_view::npos || hash == std::string_view::npos || hash < colon)
        return nullptr;
    int tile{};
    const auto digits = key.substr(colon + 1, hash - colon - 1);
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), tile);
    if (ec != std::errc{} || end != digits.data() + digits.size() || Tile(tile) != TileState::Masked ||
        key != tiles_[tile].key)
        return nullptr;
    return &tiles_[tile].mask;
}

void LiveFog::Refresh(bool active, Clock::time_point now) {
    if (!Configured()) return;
    if (!active) { Reset(); return; }
    if (!ticket_ && now < next_) return;
    // Includes request/poll, conversion and key construction, excludes the async GPU wait.
    struct Timer {
        double& us;
        Clock::time_point begin{Clock::now()};
        ~Timer() { us = std::chrono::duration<double, std::micro>(Clock::now() - begin).count(); }
    } timer{refresh_us_};
    if (ticket_) {
        EdenDsmodTextureInfo info{};
        info.struct_size = sizeof(info);
        const int status = api_.poll(api_.userdata, ticket_, pixels_.data(), pixels_.size(), &info);
        info_ = info;
        info_.status = status;
        if (status == EDEN_DSMOD_TEXTURE_PENDING) return;
        ticket_ = 0;
        if (status != EDEN_DSMOD_TEXTURE_READY || info.candidates != 1 || !info.gpu_address ||
            info.width != 256 || info.height != 256 || info.format != EDEN_DSMOD_TEXTURE_RGBA8 ||
            info.size != pixels_.size()) {
            // Any failure closes visibility. Rediscover after scene recreation or ambiguity.
            Close();
        } else if (!Ready() || info.gpu_address != address_ || info.generation != generation_) {
            const auto begin = Clock::now();
            if (!Ready())
                ++epoch_; // a reopened readback never reuses a key of the closed one
            const bool reopened = !Ready();
            const auto decoded = Decode(pixels_.data());
            std::vector<std::uint8_t> raw(std::size_t{FogSide} * FogSide);
            for (std::size_t i = 0; i < raw.size(); ++i)
                raw[i] = static_cast<std::uint8_t>(255 - decoded.rgba[i * 4 + 3]);
            address_ = info.gpu_address;
            generation_ = info.generation;
            // Outdoors the RT is redrawn every frame with unchanged pixels: nothing to rebuild.
            if (reopened || raw != raw_) {
                raw_ = std::move(raw);
                // The RT's red bytes are what the game saves: the same noise test applies.
                ce_unity::Image red{FogSide, FogSide, std::vector<std::uint8_t>(raw_.size() * 4, 255)};
                for (int y = 0; y < FogSide; ++y)
                    for (int x = 0; x < FogSide; ++x)
                        red.rgba[(std::size_t(y) * FogSide + x) * 4] =
                            pixels_[(std::size_t(FogSide - 1 - y) * FogSide + x) * 4];
                const auto trusted = TrustedFogBaseline(red);
                trusted_ = ExploredCells(&trusted, nullptr, 0);
                Compose();
            }
            cost_us_ = std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
        }
    }
    if (now < next_) return;
    next_ = now + std::chrono::seconds(1);
    // Same selector by address: a recreated or reused address that is no longer the fog RT is a
    // MISMATCH, which closes visibility and rediscovers. A host without the selector (rt24 betas
    // before 1.3.5.1-beta.4) answers INVALID: fully fogged, never an ambiguous texture.
    EdenDsmodTextureRequest request{sizeof(request), EDEN_DSMOD_TEXTURE_RGBA8, address_, 256, 256,
                                    kSelector, 0};
    int status{};
    ticket_ = api_.request(api_.userdata, &request, &status);
    if (!ticket_) info_.status = status;
    if (!ticket_ && status != EDEN_DSMOD_TEXTURE_RATE_LIMITED && status != EDEN_DSMOD_TEXTURE_BUSY)
        Close();
}
} // namespace mtas
