// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/mods/dsmod_module_abi.h"
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace xcx {
using Bytes = std::vector<std::uint8_t>;
std::uint64_t PathHash(std::string_view);
struct Image {
    int w{}, h{};
    Bytes rgba;
    Image() = default;
    Image(int width, int height);
    bool Empty() const;
};
struct Texture {
    std::size_t start{}, end{};
    std::uint32_t w{}, h{}, format{};
};
std::vector<Texture> Textures(std::span<const std::uint8_t>);
Image DecodeTexture(std::span<const std::uint8_t>, const Texture &);
class Archive {
    struct Entry {
        std::uint64_t offset;
        std::uint32_t size;
    };
    std::map<std::uint64_t, Entry> entries;
    bool initialized{};

  public:
    Bytes Read(const EdenDsmodHostApi &, std::string_view);
    // The member's bytes exactly as stored in the archive (no xbc1 decoding); empty if absent.
    Bytes ReadStored(const EdenDsmodHostApi &, std::string_view);
    // The same by the member's path hash (members whose path is not known, e.g. the title logo).
    Bytes ReadStoredHash(const EdenDsmodHostApi &, std::uint64_t);
    Bytes ReadHash(const EdenDsmodHostApi &, std::uint64_t);
};
class Assets {
    Archive archive;
    std::map<std::string, Image> images;
    Bytes messages;

  public:
    Archive &Files() { return archive; }
    const Image &Ui(const EdenDsmodHostApi &, std::string_view, int index = 0);
    // The same picture decoded and returned without being kept (map tiles: hundreds of them).
    Image UiOnce(const EdenDsmodHostApi &, std::string_view, int index = 0);
    // A UI layout member found only by its path hash (decoded, not kept).
    Image UiHash(const EdenDsmodHostApi &, std::uint64_t hash, int index = 0);
    std::string Message(const EdenDsmodHostApi &, unsigned table, unsigned row);
    // Forgets every cached image whose name starts with `prefix` when more than `max` are held
    // (map tiles: the world map has 528). Call before taking references from Ui().
    void Trim(std::string_view prefix, std::size_t max);
};
class Font {
    struct Glyph {
        unsigned index, left, width;
    };
    std::map<unsigned, Glyph> glyphs;
    Bytes bytes;
    Texture texture;
    int columns{}, pitch_y{};
    double cap{}, baseline{}, slope{};

  public:
    bool Load(Archive &, const EdenDsmodHostApi &, std::string_view);
    double Width(std::string_view, double) const;
    void Draw(Image &, int x, int baseline_y, std::string_view, double, std::uint32_t color,
              int max_width = 0, char align = 'l') const;
    bool Ready() const { return !bytes.empty(); }
};
void Blend(Image &, int, int, std::uint32_t);
void Rect(Image &, int, int, int, int, std::uint32_t);
void Blit(Image &, const Image &, int, int, int, int, int sx = 0, int sy = 0, int sw = 0,
          int sh = 0);
} // namespace xcx
