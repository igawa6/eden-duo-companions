// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// System screens of the Dragon Quest III HD-2D Remake companion (owner option A, design milestone
// 20261007T220416+0700-system-screens, research tools/render-system-screens.py start_A / loading_A /
// wrongpatch_A), composed at runtime from the player's own Nicola-Switch.pak. Each screen is one
// full-canvas module image with its text drawn in the game's Plantin MT Pro (dq3_font.h DrawGameText):
//
//   sys/start/1240x1080     title-screen parchment frame (T_UI_Title_Flame_00 nine-sliced 240 -> 150 px, the
//                           interior lifted x1.45 through a blurred rounded mask), the full logo
//                           (T_UI_TitleDemo_TitleLogo_All_02_EFIGSM) 800 px wide, "No adventure loaded" and the
//                           hint, then the march: Mage, Priest, Warrior, Hero (native walk02_L frames, the set
//                           that faces screen-right) and the Slime (battleidle01_B) as ink silhouettes (sprite
//                           alpha x 0.88 filled with (46, 32, 18)), 3x nearest, on a ground line
//   sys/loading/1240x1080   the same frame, logo 560 px, the march at 4x mid-screen, "Loading your adventure..."
//                           (state text only, no progress) and three dots
//   sys/wrong/1240x1080     the same frame, logo 620 px, the dark DQ window (T_UI_Common_Window_Base_00 at
//                           alpha x 0.88) with the "?" monster shadow, the refusal text and the OK button
//                           (T_UI_Common_Window_Base_09 + the A glyph of T_UI_Common_Control_Button_00), and the
//                           supported build in the footer
//   sys/quiet/1240x1080     after OK: the frame, logo 620 px, a two-line note and the footer
//
// Silhouettes: the alpha of each native frame, nothing else of its pixels, so the march is the same for
// every save (no party is loaded on these screens).
#pragma once

#include <optional>
#include <string_view>

#include "dq3_pak.h"

namespace dq3 {

/// Canvas of every system screen (the companion page size).
inline constexpr u32 SysW = 1240, SysH = 1080;
/// The OK button of sys/wrong (x, y, w, h), the manifest's tap rect.
inline constexpr int SysOkX = 420, SysOkY = 786, SysOkW = 220, SysOkH = 72;

bool IsSystemArtKey(std::string_view key);
/// Composes a system key (nullopt when the key is not one or a member failed its checks).
std::optional<Image> ComposeSystemArt(const RangeReader& read, std::string_view key, std::string* why = nullptr);

/// The refusal copy of sys/wrong (module case: the build is the supported one, the code pins are not).
inline constexpr std::u32string_view WrongTitle = U"This version isn't supported";
inline constexpr std::u32string_view WrongLines[3] = {U"This companion is made for Ver. 1.1.0.0.",
                                                      U"Your game's code does not match it.",
                                                      U"Remove game mods or patches to use it."};
/// sys/quiet: the wrong-patch screen after OK (frame, logo, this note, the footer).
inline constexpr std::u32string_view QuietLines[2] = {U"The companion is off for this game.",
                                                      U"Tap anywhere to show the notice again."};
inline constexpr std::u32string_view WrongFooter = U"Supported: Ver. 1.1.0.0 \u00b7 build 4F41309B39EEBE5E";

} // namespace dq3
