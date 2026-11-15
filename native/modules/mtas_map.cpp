// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mtas_map.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cmath>
#include <cstdio>

namespace mtas {

bool MarkerExplored(const ce_unity::Image* fog, const std::uint8_t* history, std::size_t size, float u, float v) {
    if (!TileAtUv(u, v)) return false;
    const auto x = std::min(255, static_cast<int>(u * 256));
    const auto y = std::min(255, static_cast<int>(v * 256));
    if (history && size == 256 * 32 && (history[x * 32 + y / 8] & (1 << (y % 8)))) return true;
    if (!fog || !fog->width || fog->width != fog->height || fog->rgba.size() != std::size_t(fog->width) * fog->height * 4) return false;
    const auto px = std::min(fog->width - 1, static_cast<unsigned>(u * fog->width));
    const auto py = std::min(fog->height - 1, static_cast<unsigned>((1 - v) * fog->height));
    return fog->rgba[(py * fog->width + px) * 4] >= 250;
}

MapTileMask TilesFromFog(const ce_unity::Image& fog) {
    MapTileMask out;
    const std::uint32_t side = fog.width;
    if (side == 0 || fog.height != side || side % MapGrid != 0 ||
        fog.rgba.size() != std::size_t{side} * side * 4)
        return out;
    const std::uint32_t cell = side / MapGrid;
    for (int row = 0; row < MapGrid; ++row) {
        for (int col = 0; col < MapGrid; ++col) {
            std::size_t revealed = 0;
            for (std::uint32_t y = 0; y < cell; ++y) {
                const std::size_t base = (std::size_t{row * cell + y} * side + col * cell) * 4;
                for (std::uint32_t x = 0; x < cell; ++x)
                    revealed += fog.rgba[base + x * 4] >= 128;
            }
            if (revealed * 2 > std::size_t{cell} * cell)
                out.set(static_cast<std::size_t>(row * MapGrid + col));
        }
    }
    return out;
}

ce_unity::Image TrustedFogBaseline(const ce_unity::Image& fog, int* rejected) {
    if (rejected) *rejected = 0;
    const std::uint32_t side = fog.width;
    if (side == 0 || fog.height != side || fog.rgba.size() != std::size_t{side} * side * 4)
        return {};
    constexpr int N = FogSide, cell = N / MapGrid;
    std::vector<std::uint8_t> red(N * N);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            if (side <= static_cast<std::uint32_t>(N)) {
                const auto sx = static_cast<std::size_t>(x) * side / N, sy = static_cast<std::size_t>(y) * side / N;
                red[y * N + x] = fog.rgba[(sy * side + sx) * 4];
                continue;
            }
            const std::size_t x0 = std::size_t(x) * side / N, x1 = std::size_t(x + 1) * side / N;
            const std::size_t y0 = std::size_t(y) * side / N, y1 = std::size_t(y + 1) * side / N;
            std::size_t sum = 0;
            for (std::size_t sy = y0; sy < y1; ++sy)
                for (std::size_t sx = x0; sx < x1; ++sx)
                    sum += fog.rgba[(sy * side + sx) * 4];
            red[y * N + x] = static_cast<std::uint8_t>(sum / ((x1 - x0) * (y1 - y0)));
        }
    int steps[MapGrid][MapGrid]{};
    bool revealed[MapGrid][MapGrid]{};
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const int v = red[y * N + x];
            const bool hard = (x + 1 < N && std::abs(v - red[y * N + x + 1]) > FogStepDelta) ||
                              (y + 1 < N && std::abs(v - red[(y + 1) * N + x]) > FogStepDelta);
            steps[y / cell][x / cell] += hard;
            revealed[y / cell][x / cell] |= v >= FogBaselineMinRed;
        }
    ce_unity::Image out{N, N, std::vector<std::uint8_t>(std::size_t{N} * N * 4, 0)};
    for (int row = 0; row < MapGrid; ++row)
        for (int col = 0; col < MapGrid; ++col) {
            int around = 0;
            for (int r = std::max(0, row - 1); r <= std::min(MapGrid - 1, row + 1); ++r)
                for (int c = std::max(0, col - 1); c <= std::min(MapGrid - 1, col + 1); ++c)
                    around += steps[r][c];
            const bool trusted = steps[row][col] <= FogTileStepMax && around <= FogNeighbourhoodStepMax;
            if (!trusted && revealed[row][col] && rejected) ++*rejected;
            for (int y = row * cell; y < (row + 1) * cell; ++y)
                for (int x = col * cell; x < (col + 1) * cell; ++x) {
                    auto* p = &out.rgba[(std::size_t(y) * N + x) * 4];
                    p[0] = p[1] = p[2] = trusted ? red[y * N + x] : 0;
                    p[3] = 255;
                }
        }
    return out;
}

std::vector<std::uint8_t> ExploredCells(const ce_unity::Image* baseline, const std::uint8_t* history,
                                        std::size_t size) {
    constexpr int N = FogSide;
    std::vector<std::uint8_t> out(N * N, 0);
    const auto disc = [&](int cx, int cy, int reach) {
        for (int y = std::max(0, cy - reach); y <= std::min(N - 1, cy + reach); ++y)
            for (int x = std::max(0, cx - reach); x <= std::min(N - 1, cx + reach); ++x)
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= reach * reach)
                    out[y * N + x] = 255;
    };
    if (baseline && baseline->width == N && baseline->height == N &&
        baseline->rgba.size() == std::size_t{N} * N * 4) {
        const auto known = [&](int x, int y) {
            return x < 0 || y < 0 || x >= N || y >= N ||
                   baseline->rgba[(std::size_t(y) * N + x) * 4] >= FogBaselineMinRed;
        };
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                if (!known(x, y))
                    continue;
                out[y * N + x] = 255;
                // Only the record's edge grows; an interior cell's disc adds nothing.
                if (!known(x - 1, y) || !known(x + 1, y) || !known(x, y - 1) || !known(x, y + 1))
                    disc(x, y, FogBaselineDilate);
            }
    }
    if (history && size == std::size_t{N} * N / 8)
        for (int x = 0; x < N; ++x)
            for (int y = 0; y < N; ++y)
                if (history[x * 32 + y / 8] & (1u << (y % 8)))
                    disc(x, N - 1 - y, FogStampReach); // history y is v (up); rows run down
    return out;
}

std::optional<int> TileAtUv(float u, float v) {
    if (!std::isfinite(u) || !std::isfinite(v) || u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f)
        return std::nullopt;
    const int col = static_cast<int>(u * MapGrid);
    const int row = MapGrid - 1 - static_cast<int>(v * MapGrid);
    if (col < 0 || col >= MapGrid || row < 0 || row >= MapGrid)
        return std::nullopt;
    return row * MapGrid + col;
}

MapTileMask TilesFromHistory(const std::uint8_t* rows, std::size_t size) {
    MapTileMask out;
    if (!rows || size != 256 * 32)
        return out;
    for (int x = 0; x < 256; ++x) {
        for (int y = 0; y < 256; ++y) {
            if (rows[x * 32 + y / 8] & (1u << (y % 8))) {
                const int col = x / (256 / MapGrid);
                const int row = MapGrid - 1 - y / (256 / MapGrid);
                out.set(static_cast<std::size_t>(row * MapGrid + col));
            }
        }
    }
    return out;
}

std::string MapTileKey(int tile, std::uint32_t px) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "module:mtas:map:%d:%d:%u", tile % MapGrid, tile / MapGrid, px);
    return buf;
}

bool ParseMapTileKey(std::string_view rest, int& col, int& row, std::uint32_t& px) {
    int parts[3]{};
    for (int i = 0; i < 3; ++i) {
        const auto end = i < 2 ? rest.find(':') : rest.size();
        if (end == std::string_view::npos || end == 0 || end > 4)
            return false;
        const auto piece = rest.substr(0, end);
        const auto [p, ec] = std::from_chars(piece.data(), piece.data() + piece.size(), parts[i]);
        if (ec != std::errc{} || p != piece.data() + piece.size())
            return false;
        rest = i < 2 ? rest.substr(end + 1) : std::string_view{};
    }
    if (parts[0] < 0 || parts[0] >= MapGrid || parts[1] < 0 || parts[1] >= MapGrid ||
        parts[2] < 32 || parts[2] > 256)
        return false;
    col = parts[0];
    row = parts[1];
    px = static_cast<std::uint32_t>(parts[2]);
    return true;
}

void MapFog::Observe(const MapSession& session) {
    if (has_session_ && session == session_id_)
        return;
    Clear();
    session_id_ = session;
    has_session_ = true;
}

void MapFog::SetBaseline(const MapTileMask& tiles) {
    baseline_ = tiles;
    baseline_ok_ = true;
}

void MapFog::Reveal(int tile) {
    if (tile >= 0 && tile < MapTiles)
        session_.set(static_cast<std::size_t>(tile));
}

void MapFog::RevealAll(const MapTileMask& tiles) {
    session_ |= tiles;
}

void MapFog::Clear() {
    session_id_ = {};
    has_session_ = false;
    baseline_ok_ = false;
    baseline_.reset();
    session_.reset();
}

} // namespace mtas
