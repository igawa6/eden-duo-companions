// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from eden-duo-companions native/modules/wonder_assets.h (Eden DSMod fork).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace KirbyAssets {

struct Image {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba;
};

using Bytes = std::vector<std::uint8_t>;
using RomfsReader = std::function<std::optional<Bytes>(const std::string&)>;

/// HAL .cmp: u32 raw size, then one zstd or LZ4 frame.
std::optional<Bytes> DecompressCmp(std::span<const std::uint8_t> in);
std::optional<Bytes> SarcMember(std::span<const std::uint8_t> sarc, std::string_view name);
std::optional<Image> DecodeBntx(std::span<const std::uint8_t> bntx, std::string_view texture);
/// Text of one MSBT label, control tags dropped.
std::optional<std::u32string> MsbtText(std::span<const std::uint8_t> msbt, std::string_view label);
/// The same text in runs of one colour (0xRRGGBB from the colour tags; 0 = the pane's own). With `glyphs`,
/// the size tags (0.2, percent) start runs of their own scale and the button tags become the button font's
/// private-use characters: 4.n its glyph U+F0xx, 7.n the action's default button (A, B, X, Y = U+F000..F003).
struct MsbtRun {
    std::u32string text;
    std::uint32_t colour{};
    float scale{1};
};
std::optional<std::vector<MsbtRun>> MsbtRuns(std::span<const std::uint8_t> msbt, std::string_view label,
                                             bool glyphs = false);
/// The text with each of its tags replaced by `fill(group, type)` (nothing when it gives nothing): the game's numbers
/// and times put in, say.
std::optional<std::u32string> MsbtFilled(std::span<const std::uint8_t> msbt, std::string_view label,
                                         const std::function<std::u32string(std::uint16_t group, std::uint16_t type)>& fill);

/// A HAL XBIN YAML document (yaml/*.bin): every offset is relative to the field that holds it.
struct YamlNode {
    enum class Kind { None, Int, Float, Bool, String, Map, List } kind{};
    std::int64_t number{};
    double real{};
    std::string text;
    std::vector<std::pair<std::string, YamlNode>> map;
    std::vector<YamlNode> list;
    const YamlNode* Find(std::string_view key) const;
};
std::optional<YamlNode> ReadXbinYaml(std::span<const std::uint8_t> bin);
/// A BFLAN's material colour curves (FLMC) for `material` at frame 0: black RGBA, then white RGBA (the layout's
/// 0..255 linear units), each missing curve -1.
std::optional<std::array<float, 8>> BflanMaterialColour(std::span<const std::uint8_t> flan, std::string_view material);
/// Every texture of a BNTX (each layer of an array texture), in its order.
std::vector<Image> DecodeBntxAll(std::span<const std::uint8_t> bntx);

/// Keys (after "module:kirby:"):
///   lyt/<dir>/<archive>/<texture>   texture from /lyt/<dir>/<archive>.arc.cmp timg/__Combined.bntx
///   gfx/<dir>/<model>/<texture>     texture from the BNTX embedded in /gfx/<dir>/<model>.bfres.cmp
/// e.g. lyt/YyCmn/StockItemInfo/Parts/StockItemIconMaximTomato^u
/// Thread-safe; decoded images and the last few archives are cached in memory only.
class Decoder {
public:
    explicit Decoder(std::size_t image_budget = 64 * 1024 * 1024);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    std::shared_ptr<const Image> Load(const RomfsReader& read, std::string_view key);
    /// romfs path and texture name for a key.
    static std::optional<std::pair<std::string, std::string>> Resolve(std::string_view key);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace KirbyAssets
