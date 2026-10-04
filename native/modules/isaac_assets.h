// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Binding of Isaac: Repentance: asset-free art. Every image is decoded at runtime from the
// player's own romfs (PCX sheets cropped by the game's .anm2 frames); nothing of the game ships.
// Called from load_image on the runtime's asset worker thread. Owner: ASSETS lane.
// The key grammar is fixed in research/isaac/CONTRACT.md ("isaac:..." without "module:").
// Keys are self-describing: an image depends only on its key (and the romfs), never on reader
// state, so the host cache can keep it.
//
//   isaac:bg/wiiu[/sheet|/scrap|/wood|/page|/sheetframe]   base:resources/wiiubottomscreen.pcx
//                                   (854x480) and its pieces; page = the mockup background at 1x
//                                   (620x540); sheetframe = the MAP box (495x466 at 1x) with the
//                                   big sheet's paper interior transparent (round 3, map clip)
//   isaac:coll/<id>  isaac:trink/<id>[/g]       items.xml icons, 32x32 (g = gold outline inside)
//   isaac:card/<id>                             ui_cardspills CardFronts frame id, 16x24
//   isaac:pocket/<kind>/<id>[/s]  isaac:pill/<color>[/s]   the HUD pocket icon (HUD 32x32,
//                                   /s = HUDSmall 16x16); kind 0 pill colour, 1 card, 2 active
//   isaac:hud/<coin|key|bomb|gkey|gbomb|ui_hearts anim>    16x16
//   isaac:stat/<0..5>[/w]                       pause-screen stat ink, 24x16 (w = the ink white)
//   isaac:charge/<cur>/<max>[/<battery>]        the HUD charge bar, 16x32
//   isaac:room/<type|IconName>                  minimap_icons, 16x16
//   isaac:map/<hex>                             the floor map, 880x800 canvas (isaac_map.h)
//   isaac:font/<name>                           BMFont page 0 (white + alpha)
//   isaac:text/<font>/<hexutf8>[/plain]         a string in a game font (+1 px black outline)
//   isaac:ui/<anm2 path>#<anim>#<frame>[#<layer id>]      one anm2 frame, its own size
//   isaac:anim/<anm2 path>#<anim>#<t>[#<overlay anim>#<t2>]   every visible layer of an anm2
//                                   animation at time t (t wraps), placed at the entity origin;
//                                   one fixed canvas per animation (round 6, loading page)
//   isaac:logo                                  the title screen's logo (titlemenu.anm2 Idle 0,
//                                   layers LogoShadow + Logo), 480x164
// The sources and code sites of every rule are in isaac_assets.cpp / isaac_map.h and
// research/isaac/assets/REPORT.md.

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace isaac_assets {

class LibraryImpl;
/// Decoded sheets + an LRU of finished images; thread-safe (the asset worker and the text thread).
class Library {
public:
    explicit Library(std::size_t image_budget = std::size_t(48) << 20);
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    LibraryImpl& impl() {
        return *p;
    }

private:
    std::unique_ptr<LibraryImpl> p;
};
std::unique_ptr<Library> CreateLibrary(std::size_t image_budget = std::size_t(48) << 20);

/// key without the "module:" prefix, e.g. "isaac:coll/1". False for unknown/undecodable keys.
bool LoadImage(Library&, const EdenDsmodHostApi* host, const char* key, std::vector<std::uint8_t>& rgba,
               std::uint32_t& w, std::uint32_t& h);

struct FontOut {
    std::uint32_t line_height{}, first_codepoint{};
    std::vector<EdenDsmodFontGlyph> glyphs;
};
/// The manifest `font` bytes (a package text file naming the game font, see CONTRACT.md) ->
/// the game's BMFont metrics; the atlas is the manifest's font_atlas module key.
bool DecodeFont(Library&, const EdenDsmodHostApi* host, const std::uint8_t* bytes, std::size_t size,
                FontOut& out);

} // namespace isaac_assets
