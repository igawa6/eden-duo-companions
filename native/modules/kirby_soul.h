// SPDX-FileCopyrightText: Copyright 2026 Fab
// SPDX-License-Identifier: GPL-3.0-or-later
// Magolor Soul's attacks crossing the Arena page (internal to the module): the game's own effect textures
// (MahoroaBattle2.ptcl) shaped into the bottom screen's tornadoes.
#pragma once

#include <cstdint>

#include "kirby_paint.h"

namespace KirbySoul {
using KirbyPaint::Image;

// MahoroaBattle2.ptcl's sand: common_scrollsand01 (the funnel's streaks), sandsmoke01 (the puffs round it), sand01
// (grains), windedge01 (its ragged rims)
struct Sand {
    const Image* scroll{};
    const Image* smoke{};
    const Image* grains{};
    const Image* edge{};
};
// A sand tornado w x h, at `loop` (0..1) of its seamless swirl
Image Tornado(const Sand& sand, float loop, std::uint32_t w, std::uint32_t h);

// MahoroaBattle2.ptcl's giant laser: scroll11 (the beam's streaks), thunder11 and thunder02 (its red bolts), beamflash02
struct Beam {
    const Image* scroll{};
    const Image* bolt{};
    const Image* bolts{};
    const Image* flash{};
};
// Magolor Soul's two giant lasers over the whole w x h page, coming down from its top: `reach` (0..1) how far they
// have come, `thin` (0..1) how far they have dwindled at the end, `loop` (0..1) their streaks rushing, `frame` their bolts
Image Lasers(const Beam& beam, float reach, float thin, float loop, int frame, std::uint32_t w, std::uint32_t h);

// The left giant laser as the page shows it, for a picture w x h of the page's box x -100..620, y 0..1340 (its heads
// below the page's bottom, sliding down with it): `thin` (0..1) dwindling, `loop` its streaks, `frame` its bolts; the
// right one is its mirror
Image LaserLeft(const Beam& beam, float thin, float loop, int frame, std::uint32_t w, std::uint32_t h);

// Magolor's hand circle (MahoroaBattle2.ptcl's mahoroabattle2handring, its top-left quarter: the runic ring round the
// corner, two spikes outside) whole, mirrored four ways, lit white and glowing, size x size
Image HandRing(const Image& quarter, std::uint32_t size);

// Where the ground between the beams has burnt at `t` seconds after the beams passed row y: 0 untouched, then charring
// (0..0.5), embers (0.5..1), 1 burnt through. The fire eats in from the V's sides to its middle, its front ragged
// (`noise`, a cloudy texture, 0..1 at x, y).
float Burnt(float x, float y, float t, float page_w, float page_h, const Image& noise);
// The page's ground at a burning point: `ground` charred and glowing over `sky` by Burnt's value
KirbyPaint::Rgba Burning(KirbyPaint::Rgba ground, KirbyPaint::Rgba sky, float burnt);

} // namespace KirbySoul
