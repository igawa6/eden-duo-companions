// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kirby_assets.h"

// The world map's planets (gfx/WMap/Map/<planet>/Bg), drawn from their models in software: seen from
// above, leaning 40 degrees towards the north, on a clear background.
namespace KirbyPlanet {
using KirbyAssets::Image;

enum class Planet { PopStar, Halcandra };

// a texture of the model's own BNTX by its full name (hash suffix included)
using Textures = std::function<std::shared_ptr<const Image>(const std::string&)>;

// where a marker bone (Lv<n>MedalL, ShipInfoL) lands in the picture
struct Anchor {
    std::string bone;
    float x{}, y{};
};

std::string ModelPath(Planet p); // romfs path of the compressed model

struct Scene;

class Model {
public:
    Model(Planet p, std::span<const std::uint8_t> bfres);
    ~Model();
    bool Valid() const;
    // the planet in a size x size picture (each pixel `ss` x `ss` samples)
    std::optional<Image> Draw(const Textures& tex, std::uint32_t size, int ss) const;
    std::vector<Anchor> Anchors(std::uint32_t size) const;

private:
    std::unique_ptr<Scene> scene;
};

} // namespace KirbyPlanet
