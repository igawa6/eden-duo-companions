// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "chained_echoes_unity.h"
#include "core/mods/dsmod_module_extensions.h"
#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace mtas {
// One asynchronous request per second. No configure call preserves the rt23 tile path.
//
// What is shown is the readback AND the companion's explored record: SetExplored (the save's
// trusted baseline + this session's stamps, mtas_map.h ExploredCells) OR the readback's own
// tiles that pass the same noise test (so a genuine fog shows exactly as the game draws it, also
// reveals that came from neither stamps nor the loaded save). A cell the RT reveals that neither
// knows stays fogged: the uninitialised-memory stripes of a poisoned save (never more than native).
// The gated reveal is published twice:
//  - Key()/Image(): FogSide^2, alpha = fog, the mask of the fog-art overlay over the whole map;
//  - per MapGrid tile: Tile(i) Hidden (nothing revealed: not drawn), Full (drawn unmasked) or
//    Masked (TileMaskKey(i)/TileMask(i): TileMaskPx^2, the tile's own bilinear crop of the gated
//    reveal, sampled where the host's stretch puts each mask pixel, so neighbouring tiles meet).
// The tile overlays carry the native 4096 px map art (256 px a tile at detail zoom), so a reveal
// keeps full resolution at max zoom without a 64 MiB world picture.
// Keys change only when their pixels do; the RT's generation alone advances every frame outdoors.
class LiveFog {
public:
    using Clock = std::chrono::steady_clock;
    // The fog RT is the only 256x256 RGBA8 render target the game keeps drawing into: sRGB and
    // redrawn in later frames. Gameplay also keeps 10+ same-size one-off render targets resident,
    // so discovery without these selector flags is ambiguous (Eden Duo 1.3.5.1-beta.4+ host).
    static constexpr std::uint32_t kSelector = EDEN_DSMOD_TEXTURE_REQUIRE_RENDER_TARGET |
                                               EDEN_DSMOD_TEXTURE_REQUIRE_SRGB |
                                               EDEN_DSMOD_TEXTURE_REQUIRE_REDRAWN;
    void Configure(const EdenDsmodHostTextureApi* api);
    void Reset();
    void Refresh(bool active, Clock::time_point now = Clock::now());
    bool Configured() const { return api_.request != nullptr; }
    bool Ready() const { return !key_.empty(); }
    const std::string& Key() const { return key_; }
    const ce_unity::Image& Image() const { return image_; }
    static constexpr std::uint32_t TileMaskPx = 32;
    enum class TileState : std::uint8_t { Hidden, Full, Masked };
    TileState Tile(int tile) const;
    const std::string& TileMaskKey(int tile) const;
    const ce_unity::Image& TileMask(int tile) const;
    /// The image a current key names (Key() or a TileMaskKey), or null (stale / unknown key).
    const ce_unity::Image* Find(std::string_view key) const;
    /// FogSide^2 explored cells (255 = explored), row 0 = map top; anything else = none explored.
    void SetExplored(std::vector<std::uint8_t> cells);
    bool HasExplored() const { return !explored_.empty(); }
    /// The shown (gated) reveal 0..255 at a normalised map point (u right, v up); 0 when closed.
    int RevealAt(float u, float v) const;
    /// Cells the readback reveals that the explored record does not (shown fogged).
    int Gated() const { return gated_; }
    std::uint64_t Generation() const { return generation_; }
    double CostUs() const { return cost_us_; }
    double RefreshUs() const { return refresh_us_; }
    const EdenDsmodTextureInfo& Info() const { return info_; }
    static ce_unity::Image Decode(const std::uint8_t* rgba);
    /// TileMaskPx^2 bilinear crop of a FogSide^2 reveal (row 0 = top) for MapGrid tile `tile`.
    static std::vector<std::uint8_t> TileCrop(const std::vector<std::uint8_t>& reveal, int tile);
private:
    void Close();
    void Compose();
    EdenDsmodHostTextureApi api_{};
    EdenDsmodTextureInfo info_{};
    std::uint64_t ticket_{}, address_{}, generation_{}, epoch_{};
    Clock::time_point next_{};
    std::array<std::uint8_t, 256 * 256 * 4> pixels_{};
    ce_unity::Image image_;
    std::vector<std::uint8_t> raw_;      ///< readback reveal, FogSide^2, row 0 = top
    std::vector<std::uint8_t> explored_; ///< FogSide^2 or empty
    std::vector<std::uint8_t> trusted_;  ///< FogSide^2: the readback's own noise-free cells
    std::string key_;
    std::uint64_t revision_{};
    int gated_{};
    struct TileMaskState {
        TileState state{TileState::Hidden};
        ce_unity::Image mask;
        std::string key;
        std::uint64_t revision{};
    };
    std::vector<TileMaskState> tiles_;
    double cost_us_{}, refresh_us_{};
};
} // namespace mtas
