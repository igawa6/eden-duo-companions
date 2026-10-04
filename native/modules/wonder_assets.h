// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"

namespace WonderAssets {

struct Image {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba;
};

struct AstcDecoder {
    void* userdata{};
    EdenDsmodAstcDecoder decode{};
};

/// Reads a romfs file (the module passes host->read_romfs; the test tool reads a dump).
using RomfsReader = std::function<std::optional<std::vector<std::uint8_t>>(const std::string&)>;

/// zstd frame -> bytes (input returned unchanged when it is not a zstd frame).
std::optional<std::vector<std::uint8_t>> Zstd(std::span<const std::uint8_t> in);
/// SARC member by name.
std::optional<std::vector<std::uint8_t>> SarcMember(std::span<const std::uint8_t> sarc,
                                                    std::string_view name);
std::vector<std::string> SarcNames(std::span<const std::uint8_t> sarc);
/// Texture names in a BNTX.
std::vector<std::string> BntxNames(std::span<const std::uint8_t> bntx);
/// Decode one BNTX texture (by name; empty name = the first texture) to RGBA8.
std::optional<Image> DecodeBntx(std::span<const std::uint8_t> bntx, std::string_view texture,
                                AstcDecoder astc);

/// Logical image keys served to the manifest as module:wonder:<key>:
///   chara/<0..11>        character icon (ids as published in wonder.character)
///   power/<1|2|3|6|9>    power-up / reserve item icon
///   course/<NNN>         course thumbnail
///   lyt/<Layout>/<Tex>   a texture from /Layout/<Layout>.Nin_NX_NVN.blarc.zs timg/__Combined.bntx
///   icon/<Name>          /UI/Tex/Icon/<Name>.bntx.zs
/// (The module also serves pict/, blur/, gen/ and font/ keys itself, outside this decoder.)
/// Thread-safe, cached (LRU by bytes). Nothing decoded is persisted or shipped.
class Decoder {
public:
    explicit Decoder(std::size_t cache_budget = 48 * 1024 * 1024);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    std::shared_ptr<const Image> Load(const RomfsReader& read, std::string_view key);
    void SetAstcDecoder(AstcDecoder decoder);
    /// romfs path + texture name a key resolves to (empty when the key is invalid).
    static std::optional<std::pair<std::string, std::string>> Resolve(std::string_view key);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace WonderAssets
