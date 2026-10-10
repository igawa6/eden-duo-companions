// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Dragon Quest III HD-2D Remake 1.1.0.0: class sprites, job names and the HP-state text colours,
// all taken at runtime from the player's own Nicola-Switch.pak through pinned members (dq3_pak.h).
//
// Sprites. The game names a party member's looks row with GetLooksId (main+0x8d0380, the same
// call the menus make with "coffin when dead" and "costume" on):
//   dead (F+0x8C bit 0)        -> "UNIT_LOOKS_PC_COFFIN_<CLASS>"
//   otherwise                  -> "UNIT_LOOKS_PC_<CLASS>_<C+0xB8 as %02d>", plus "_MIZUGI" (or the
//                                 whole id "UNIT_LOOKS_PC_NUIGURUMI_00" for the Cat Suit) when the
//                                 equipped armour has a COSUTUME_CHANGE spec
// with <CLASS> = the FString table at main+0x5625440+0xe08 indexed by C+0xAA - 1 (HERO, WARRIOR,
// FIGHTER, MAGE, PRIEST, MERCHANT, GADABOUT, THIEF, SAGE, BREEDER, read live).
// Data/DataAsset/DataAssetGOPUnitLooks maps the row to a character blueprint folder; the idle
// "Stop" flipbook (000/Stop, key frame 0, BOTTOM) is <folder>/Frames/wait01_B, a PaperSprite whose
// BakedSourceTexture / BakedSourceUV / BakedSourceDimension name the B8G8R8A8 atlas and the frame.
// research tools/dq3-sprite-pins.py walks that chain offline and writes dq3_sprite_pins.inc
// (members, hashes, byte offsets of the UV / dimension floats). At runtime the sprite member is
// read and hash-checked, the rect is taken from its floats, and only that rect of the atlas is
// converted (SpriteFrame caches the recent frames).
//
// ComposeSprite(index) (index into SpritePins(); drawn inside the fcard/ top card, dq3_info.h): the
// frame at 2x nearest (never filtered), cropped to SpriteMaxH rows from the top, centred like the
// revision 2 field card
// (render.py field_card: x + 10 + (80 - w) / 2, y + (188 - h) / 2, truncated) on
// a SpriteBoxW x SpriteBoxH transparent canvas whose origin is SpriteBoxDx px left of the card's
// inner padding.
//
// Job names: GetJobTextId (main+0xce2740) maps C+0xAA 1..10 to Txt_Status_Job_<Hero, Warrior,
// MartialArtist, Mage, Priest, Merchant, Gadabout, Thief, Sage, MonsterTamer>; the hero becomes
// Txt_Status_Job_Hero_Roto when flag 0x8b of the bitset at P+0x128+0xC8 is set (0x84a858). The
// menu label is GOP_Text_Noun_ENGLISH row "TEXT_NOUN_ENGLISH_" + id without "Txt_", column
// ListNoun (contracts/INVENTORY.md).
//
// Colours: the menus' text material MT_UI_TextColor takes the HP state as Status_Value and picks
// Color_<state>_Top (main+0xc9be38: state = 2 when dead, else the state of main+0xce24b0). The
// LinearColor defaults are read from the material's cached expression data and converted to 8-bit
// sRGB (round to nearest), e.g. Color_4_Top (1, 0.74282, 0) -> (255, 224, 0), as measured on the
// native menu.
#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "dq3_pak.h"

namespace dq3 {

struct SpritePin {
    PakMember sprite;  ///< <folder>/Frames/wait01_B.uexp (PaperSprite)
    PakMember texture; ///< its BakedSourceTexture .uexp (PF_B8G8R8A8)
    u32 tex_w, tex_h;
    u32 data_at; ///< informational (DecodeTexture checks the layout)
    u32 uv_at;   ///< byte offset of BakedSourceUV (2 x float) in the decoded sprite member
    u32 dim_at;  ///< byte offset of BakedSourceDimension (2 x float)
    u32 rect[4]; ///< x, y, w, h as decoded offline (the runtime decode must agree)
};
struct LooksRow {
    std::string_view id; ///< GOP_Unit_Looks row name, sorted
    u32 sprite;          ///< index into SpritePins()
};

std::span<const SpritePin> SpritePins();
std::span<const LooksRow> LooksRows();
/// Sprite index of a looks row, nullopt when the row has no pinned sprite.
std::optional<u32> FindLooks(std::string_view looks_id);

inline constexpr int SpriteScale = 2, SpriteMaxH = 168; // the card height - 20
inline constexpr int SpriteBoxDx = 20, SpriteBoxW = 120, SpriteBoxH = 188; // revision 2 card 286 x 188

/// The frame rectangle read from the sprite member (checked against the pin). `why` on failure.
std::optional<std::array<u32, 4>> SpriteRect(std::span<const u8> sprite_uexp, const SpritePin& pin,
                                             std::string* why = nullptr);
/// Places the frame `frame` (straight RGBA) on the card canvas (see the header comment).
Image SpriteCanvas(const Image& frame);
/// The idle frame of class sprite `index` (straight RGBA, 1x). Only the frame rectangle of the atlas
/// is converted; recent frames are cached (thread-safe, 16 entries).
std::shared_ptr<const Image> SpriteFrame(const RangeReader& read, u32 index, std::string* why = nullptr);
/// Reads, checks and composes the card canvas of sprite `index` (see the header comment).
std::optional<Image> ComposeSprite(const RangeReader& read, u32 index, std::string* why = nullptr);

/// The ten Txt_Status_Job_* ids by vocation byte (1..10) and the hero's Roto variant.
std::optional<std::string_view> JobTextId(std::uint8_t vocation, bool roto);

/// ListNoun of every TEXT_NOUN_ENGLISH_Status_Job_* row (key = text id with "Txt_").
using JobNames = std::map<std::string, std::u32string, std::less<>>;
/// Parses the cooked DataTable (uasset name map + uexp rows); nullopt on any format surprise.
std::optional<JobNames> ParseJobNames(std::span<const u8> uasset, std::span<const u8> uexp);

struct TextColours {
    u32 dead{}, low{}, half{}; ///< ARGB of Color_2/3/4_Top: dead, HP <= 30 %, HP <= 50 %
};
std::optional<TextColours> ParseTextColours(std::span<const u8> material_uexp);

/// Job names and colours from the archive, built once (thread-safe; nullptr until built).
struct GameText {
    JobNames jobs;
    TextColours colours;
};
std::shared_ptr<const GameText> LoadGameText(const RangeReader& read, std::string* why = nullptr);
std::shared_ptr<const GameText> CachedGameText();

/// The pinned GOP_Text_Noun_ENGLISH members (uasset, uexp).
const PakMember& NounAssetMember();
const PakMember& NounExpMember();

/// 8-bit sRGB of a linear channel (UE / GPU encode, round to nearest).
u8 LinearToSrgb8(float linear);

} // namespace dq3
