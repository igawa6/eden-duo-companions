// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Shared chrome primitives of the ACNH art recipes (acnh_mock.py "shared chrome"), used by
// acnh_ui_pages.cpp and acnh_ui_bg.cpp. Internal to the core lane.
#pragma once

#include <string_view>

#include "acnh_draw.h"
#include "acnh_ui_art.h"

namespace acnh::ui {

inline constexpr int CanvasW = 1240, CanvasH = 1080;
inline constexpr int Top = 12;  ///< content top (round 2: tab bar moved to the bottom)
inline constexpr int Bar = 112; ///< bottom tab bar height

inline Rgba C(std::string_view hex) {
    return draw::Color(hex);
}
inline constexpr std::string_view Cream = "fff1c3"; ///< LPocketBtn / LMapBtn / PocketMenu cream
inline constexpr std::string_view SeaLive =
    "7fd8c1"; ///< LIVE sea (MenuMapFull P_Base_00 renders lighter)

/// LMenuDeviceBtn#DeviceBtnBase^s (NookPhone app squircle) 9-sliced (52 px corners -> r), tinted.
Image Card(Art& art, double w, double h, Rgba colour, double r = 44);
/// LBookCatBtnS_00 W_Base_00 window: CmnGuideCorner^s caps, tinted.
Image Capsule(Art& art, double w, double h, Rgba colour);
/// LPocketBtn#Maru128_00^s circle.
Image Disc(Art& art, double d, Rgba colour);
/// LArrowBtn#ListArrow^s + its vertical flip = chevron (mirrored for the left side).
Image Chevron(Art& art, Rgba colour, double h, bool right);
/// Orange selected-slot stripes: LPocketBtn#DeviceAppPattern04^r b #ffbb00 w #f38500, tiled at
/// `period` px, masked by `shape`.
Image Stripes(Art& art, const Image& shape, double period = 40);
/// round-2 page background (LCmnBg): cream #fff1c3 + wash #bfa46738 + CmnBgDot^s #fff4c2 tiled.
Image LcmnBg(Art& art, int w, int h);
/// Full-content panels of the Map / Critters / DIY pages (w x h; no shadow).
Image PanelSea(Art& art, int w, int h);
Image PanelBook(Art& art, int w, int h);
Image PanelPaper(Art& art, int w, int h);

} // namespace acnh::ui
