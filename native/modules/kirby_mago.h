// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "kirby_modes.h"

// Merry Magoland's plaza page (the user's sketch, 2026-11-04): a cream striped page under a blue title bar (the missions
// list's MissionListTitle^u, mirrored across the width), the section's name with its page dots under it, the yellow
// arrows that change section and the missions count on the right. The attractions' doors are KirbyGates' pictures.
namespace KirbyMago {
using KirbyAssets::Image;

constexpr int Sections = 4; // Minigames, Missions, Masks, Cards
constexpr std::array<std::uint32_t, 2> ArrowSize{116, 116}, TitleSize{560, 130}, DotsSize{164, 36}, MissionsSize{360, 128};

std::optional<Image> Background(const KirbyModes::Sources& src);
std::optional<Image> Arrow(const KirbyModes::Sources& src, bool right);
// the section's name, as large as the longest of the four allows (every section's name the same size)
std::optional<Image> Title(const KirbyModes::Sources& src, const std::u32string& name, const std::array<std::u32string, Sections>& names);
std::optional<Image> Dots(const KirbyModes::Sources& src, int current);
// "Missions" over the yellow ribbon with the count: done / total
std::optional<Image> Missions(const KirbyModes::Sources& src, const std::u32string& label, int done, int total);

// The Missions section (the user's sketch 2026-11-09): the game's mission list (YyCmn/MissionList) on the page, a set of
// missions at a time (the ten attractions, the extra and the true extra missions) under a set bar, the missions in rows
// beside a scrollbar, the selected one's card (Completion Criteria) on the right; the pins the user added (at most five a
// file, pinned missions first in their set).
constexpr int SetCount = 12;
constexpr float ListScale = 0.66f; // the list's rows at 0.66 of the game's layout units
constexpr std::array<std::uint32_t, 2> SetBarSize{900, 92}, SetArrowSize{60, 76}, PinCountSize{130, 72}, SetDotsSize{420, 40},
    ClearSize{180, 44}, RowSize{652, 100}, PinSize{56, 56}, ThumbSize{24, 240}, CardSize{480, 650};
constexpr float CardScale = 0.69f, CardTop = 28; // the card at 0.69 of its layout units, its paper 28 px down
std::optional<Image> SetBar(const KirbyModes::Sources& src, const std::u32string& name);
std::optional<Image> SetArrow(const KirbyModes::Sources& src, bool right);
// the pin and n / 5
std::optional<Image> PinCount(const KirbyModes::Sources& src, int pinned);
std::optional<Image> SetDots(const KirbyModes::Sources& src, int count, int current);
std::optional<Image> ClearCount(const KirbyModes::Sources& src, int done, int total);
// a mission's row; the selected one moved right with its frame and star; a done one with its stamp
std::optional<Image> MissionRow(const KirbyModes::Sources& src, const std::u32string& title, bool selected, bool done);
// the pin on a row (pinned) or its socket
std::optional<Image> Pin(const KirbyModes::Sources& src, bool pinned);
std::optional<Image> ScrollThumb(const KirbyModes::Sources& src, std::uint32_t h);
std::optional<Image> ScrollTrack(const KirbyModes::Sources& src, std::uint32_t h);
struct CardText {
    std::u32string headline, criteria, comment;
    int difficulty{1}; // 1..3, Magolor's badge
    bool done{};
};
// the selected mission's card, on its stack of papers with the clip
std::optional<Image> MissionCard(const KirbyModes::Sources& src, const CardText& text);

// The Masks section (the user's sketch 2026-11-11): the game's Dress-Up Masks buttons (YyCmn/OmenSelect, the icons from
// YyCmn/OmenSs) in the game's misaligned rows, rearranged to fit the page; a tap wears the mask.
constexpr float MaskScale = 0.9f; // the buttons at 0.9 of their layout units
constexpr std::uint32_t MaskPicture = 128;
// Cmn.Hero.OmenKind's masks in their order (1..86, 0 none): the select screen's order and its icons' names (OmenImg_<name>)
constexpr std::array<std::string_view, 86> MaskNames{
    "Kirby", "KirbyBlue", "KirbyGreen", "KirbyYellow", "Dedede", "Metaknight", "BandanaWaddledee", "Mahoroa", "Waddledee", "WaddleDoo",
    "Kracko", "MisterFrosty", "Bonkers", "NightmareWizard", "Rick", "Pick", "Kine", "Mine", "Coo", "CooGray", "DarkMatterClone",
    "ChefKawasaki", "Marx", "MarxSoul", "GalacticNova", "Nago", "Pitch", "Chuchu", "Gooey", "MockMatter", "Zero", "Adeleine", "Adeleine2",
    "Ribbon", "Ribbon2", "ShadowKirby", "DarkMetaknight", "DarkMirror", "Drawcia", "Daroach", "DarkDaroach", "DarkNebula", "MaskedDedede",
    "Galacticknight", "Whispywoods", "WhispywoodsEx", "Doubter", "DoubterEx", "FattyBuffer", "FattyBufferEx", "Goliath", "GoliathEx",
    "GrandLowper", "GrandLowperEx", "MetalGeneral", "MetalGeneralEx", "Landia", "LandiaEx", "MahoroaBattle1", "MahoroaBattle1Ex", "Taranza",
    "Sectonia", "QueenSectonia", "RobobotArmor", "Susie", "SusieHoliday", "Haltmann", "FrozenGeneral", "FrozenGeneralAnother",
    "BlazingGeneral", "BlazingGeneralAnother", "LightningGeneral", "LightningGeneralAnother", "Hiness", "Morphoknight", "DeformCone",
    "Awoofy", "Clawroline", "WildDedede", "Leongar", "Elfilin", "ElfilinSmile", "MahoraWhite", "CastWaddleDee", "MahoroaManager",
    "KirbyGold"};
constexpr int MaskSlots = static_cast<int>(MaskNames.size()) + 1; // Remove first
// the game's rows (YyCmnOmenSelectBase: 117.9 units apart, lines 109.4 down, every other one 36 further right) at the
// buttons' scale, 11 a row; a slot's centre on the page (gen_manifest MASK_*)
constexpr int MaskColumns = 11;
constexpr float MaskPitch = 117.9f * MaskScale, MaskLine = 109.4f * MaskScale, MaskShift = 36 * MaskScale, MaskLeft = 73.25f, MaskTop = 256;
constexpr std::array<float, 2> MaskCentre(int slot) {
    const int row = slot / MaskColumns, col = slot % MaskColumns;
    return {MaskLeft + static_cast<float>(row % 2) * MaskShift + static_cast<float>(col) * MaskPitch, MaskTop + static_cast<float>(row) * MaskLine};
}
// a mask's button: its icon on its disc, the voice mark when it speaks; with `player` ("P1") the player's frame and tag
// round it, the mask worn
std::optional<Image> MaskButton(const KirbyModes::Sources& src, const std::string& icon, bool voice, const std::u32string& player);
// a mask not got yet: the disc with the game's mark ("?")
std::optional<Image> MaskLocked(const KirbyModes::Sources& src, const std::u32string& mark);
// Remove: the disc with the dashed ring (the frame round it when no mask is worn)
std::optional<Image> MaskRemove(const KirbyModes::Sources& src, const std::u32string& player);

// The Cards section (the user's sketch 2026-11-11): the stamp rally's current card as the game shows it (YmEntrance/StampRally,
// the card part with its route part), and under it a blue bar with the stamps got and the cards finished.
constexpr float CardShown = 0.67f; // the card's 1920 x 1080 layout units at 0.67
constexpr std::array<std::uint32_t, 2> StampCardSize{1286, 724}, StampBarSize{1080, 110};
struct StampLook {
    int illustration{1}; // 1..6
    int colour{1};       // 1..5: Card<n> and the route's Card<n>Color (5: the platinum pattern)
    int rarity{4};       // the frame: 0 Platinum, 1 Gold, 2 Silver, 3 Bronze, 4 Normal
    bool bonus{};        // Route4: four chests on the way
    bool crown{};
    int stamps{};        // 0..15
    std::u32string number; // "No. 01"
};
// the looks the game's panel gives the save's card (validated 2026-11-11 against the game's panel on cards 1, 60, 99 and
// past it): `card` its number (99 at most), `after_max` the cards past the 99th (-1 on the first 99th, 0 again after each
// bonus card), `stamps` on it
StampLook StampRules(int card, int after_max, int stamps);
std::optional<Image> StampCard(const KirbyModes::Sources& src, const StampLook& look);
std::optional<Image> StampBar(const KirbyModes::Sources& src, int stamps, int cards);

// The page while an attraction's match is played (the user's sketch 2026-11-11): the attraction's ticket on the title bar,
// a box with the pinned missions (the Missions section's rows and card) and one with the Magoland Tour's attractions, the
// one being played framed in red; each box tabbed with its icon and name, a line in the title screen's "Press A" font when
// it has nothing to show.
// (the cursor's rim lies on the card's edge)
constexpr std::array<std::uint32_t, 2> GameTicketSize{580, 210}, PinsBoxSize{1220, 600}, TourBoxSize{1220, 262}, TourCardSize{164, 160},
    TourArrowSize{28, 38}, TourCursorSize{178, 174};
constexpr int TourPitch = 232; // the tour's cards apart on the page (gen_manifest GAME_*)
// the plaza marker's ticket with the attraction's title (Name/YySubGame's <kind>) in that attraction's own lettering
// (`kind`: ScnContext.Yy.YySubGameKind)
std::optional<Image> GameTicket(const KirbyModes::Sources& src, int kind, const std::u32string& title);
// `tour`: the Magoland Tour's flag on the tab, else the pin; `empty` centred when the box has nothing in it
std::optional<Image> GameBox(const KirbyModes::Sources& src, std::array<std::uint32_t, 2> size, bool tour, const std::u32string& name,
                             const std::u32string& empty);
// an attraction's card as the tour's own (YmGrandPrixTourSelectThumbnail): its name over its picture (GameSs.<kind>)
std::optional<Image> TourCard(const KirbyModes::Sources& src, int kind, const std::u32string& name);
std::optional<Image> TourArrow(const KirbyModes::Sources& src);
std::optional<Image> TourCursor(const KirbyModes::Sources& src);

} // namespace KirbyMago
