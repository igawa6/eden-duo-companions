// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "fe3h_ui2d.h"
#include "core/mods/dsmod_module_extensions.h"
namespace SmoUi {
/// Generic ASTC4x4 sRGB BRTI texture, decoded through the host's ASTC decoder extension (the
/// same EdenDsmodAstcDecoder service smo_capture.cpp already uses for capture portraits, here
/// generalised to arbitrary width/height instead of a hardcoded 256x256 -- fe3h_ui2d::BntxDecode
/// only supports BC1/BC3/BC4/BC5, and both TitleLogoSmallParts and CursorParts (the owner's
/// 2026-10-23 list-wash fix request) ship as ASTC4x4_SRGB, confirmed this session by decoding
/// the real BNTX headers.
std::optional<fe3h_ui2d::Texture> DecodeAstcBrti(std::span<const uint8_t> bntx,
                                                 std::string_view name, void* userdata,
                                                 EdenDsmodAstcDecoder decode);
std::optional<fe3h_ui2d::Texture> Life(std::span<const uint8_t> bntx, unsigned current, unsigned maximum);
std::optional<fe3h_ui2d::Texture> WorldBackground(std::span<const uint8_t> bntx);
/// Same CmImageWorld03 crop as WorldBackground, but neutral grayscale (the raw mask
/// luminance, not the title/loading page's hardcoded red colour ramp) -- for a page that
/// multiplicatively tints the world art to an arbitrary colour (03-list-wash), tinting the
/// already-red WorldBackground would produce a muddy, mostly-unrecoverable result for any
/// kingdom colour that isn't itself red/gold.
std::optional<fe3h_ui2d::Texture> WorldBackgroundNeutral(std::span<const uint8_t> bntx);
std::optional<fe3h_ui2d::Texture> FlagRing(std::span<const uint8_t> bntx);
std::optional<fe3h_ui2d::Texture> ObjectivePointer(std::span<const uint8_t> bntx, unsigned degrees);
std::optional<fe3h_ui2d::Texture> ObjectiveDot(std::span<const uint8_t> bntx);
fe3h_ui2d::Texture Slab(bool tall = false);
/// Flat white 4x4 fill, for a `bar` widget's `image` field (PACKAGE_FORMAT.md Sec 3.15: `tint`
/// / `tint_bind` apply to "image, pips, bar `image`", so this plain swatch plus `tint_bind` is
/// how a bar gets a per-kingdom colour -- the `color` field itself has no bind). Owner request
/// 2026-10-23 (globe page per-kingdom bar colour); stretch-filled by the runtime, so size is
/// arbitrary and kept tiny.
fe3h_ui2d::Texture BarFill();
/// TitleLogoSmallParts.szs CmTitleLogoSmall^w, 240x150, used as-is (the accepted list-wash
/// mockup's small logo). ASTC4x4 -- needs the host decoder.
std::optional<fe3h_ui2d::Texture> TitleLogoSmall(std::span<const uint8_t> bntx, void* userdata,
                                                 EdenDsmodAstcDecoder decode);
/// CursorParts.szs CmCursor00^w + CmCursorEyes00^w composited (the accepted mockups' idle
/// Cappy convention): an 80x60
/// canvas, cursor body at (0,4), eyes at (40,2). ASTC4x4 -- needs the host decoder.
std::optional<fe3h_ui2d::Texture> CappyCursor(std::span<const uint8_t> bntx, void* userdata,
                                              EdenDsmodAstcDecoder decode);
}
