// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The independent bottom-screen Pokétch: app screens composed from the game's own Pokétch art
// (UIs/ui/uiresidentwindow, pkc_* sprites) the way the PoketchWindow prefab lays them out, then
// shown through the display's own look: greyscale art multiplied by the Color Changer colour and
// a subtle8px square-grid overlay requested by the owner. Native640x480
// artwork is preserved without coarse dot averaging. The game's watch is never opened or read from the
// screen; every value comes from the module (save data, party, the console clock, app state).
//
//   module:lp:poketch/<app>/<colour>/<args...>    a 640x480 LCD image
//
// Apps follow the game's app numbers (0 Digital Watch .. 19 Hidden Moves). Arguments per app are
// documented at Render().

#pragma once

#include "lp_unity.h"

#include <array>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lp_poketch {

// The apps' names are the game's dp_poketch labels (lp_strings poketch_app_<id>).
inline constexpr int Width = 640, Height = 480, AppCount = 20;

/// The game's built-in Dot Artist picture ("Touch!") shown until the player first draws.
std::vector<std::uint8_t> DefaultDotArt();
/// Unpack the save's 192-byte dotart_data into 768 dot values 0-3 (row 0 at the top).
std::vector<std::uint8_t> UnpackDotArt(const std::uint8_t* data);

enum App : int {
    DigitalWatch = 0, Calculator = 1, Pedometer = 3, PokemonList = 4, Friendship = 5, Counter = 9,
    AnalogWatch = 10, MarkingMap = 11, CoinToss = 12, Calendar = 13, ColorChanger = 18,
    MemoPad = 2, Roulette = 15, KitchenTimer = 17, Dowsing = 6, EggMonitor = 7, History = 8,
    DotArtist = 14, ChainCounter = 16, HiddenMoves = 19,
};

// Finite, quantized touch motion keeps image-cache churn bounded. Negative time
// and completed motion both return the resting pose.
inline constexpr int CoinFrame(std::int64_t elapsed_ms) {
    return elapsed_ms < 0 || elapsed_ms >= 960 ? -1 : static_cast<int>(elapsed_ms / 80);
}
inline constexpr int HopOffset(std::int64_t elapsed_ms) {
    constexpr int offset[] = {0, -12, -24, -12, 0};
    return elapsed_ms < 0 || elapsed_ms >= 400 ? 0 : offset[elapsed_ms / 80];
}

/// Drawing surfaces the module edits by touch (Memo Pad cells, the Roulette wheel marks). The
/// image key carries a revision, so the renderer reads the current cells from this store.
enum Surface : int { MemoSurface = 0, RouletteSurface = 1, DotArtSurface = 2 };
inline constexpr int MemoCols = 51, MemoRows = 45, MemoCell = 10;      // the 510x450 memo canvas
inline constexpr int WheelCols = 23, WheelRows = 23, WheelCell = 20;   // the 460x460 wheel
void SetSurface(int id, std::vector<std::uint8_t> cells);
/// Empties the store; a new module session starts with blank surfaces.
void ResetSurfaces();

/// The eight Color Changer colours (PoketchWindow._bgColors), RGB.
/// Index 8 is Platinum's own teal display, used for the default colour in the classic theme.
inline constexpr std::array<std::array<std::uint8_t, 3>, 9> Colours{{
    {111, 174, 106}, {199, 194, 101}, {199, 160, 82}, {212, 108, 108},
    {148, 121, 199}, {114, 143, 186}, {121, 194, 199}, {173, 173, 173}, {92, 186, 196}}};

/// Calculator keys: glyph code, centre offset from (320, 284) with y up, width (height 88).
struct CalcKey { int code, x, y, w; };
inline constexpr CalcKey CalcKeys[17] = {
    {0, -156, -144, 200}, {10, 0, -144, 96}, {16, 156, -144, 200}, {1, -208, -48, 96}, {2, -104, -48, 96},
    {3, 0, -48, 96}, {14, 104, -48, 96}, {15, 208, -48, 96}, {4, -208, 48, 96}, {5, -104, 48, 96},
    {6, 0, 48, 96}, {12, 104, 48, 96}, {13, 208, 48, 96}, {7, -208, 144, 96}, {8, -104, 144, 96},
    {9, 0, 144, 96}, {11, 156, 144, 200}};

/// Full-screen display scale (x1000): 8 px dots become exactly 15 px, 640x480 becomes 1200x900.
inline constexpr int FullScale = 1875;

/// Calculator glyph codes (pkc_txt_num_02_32x36_NN): 0-9 digits, 10 '.', 11 C, 12 +, 13 -,
/// 14 x, 15 /, 16 =, 17 no operator, 18 error.
struct Calc {
    std::string entry = "0";  ///< what the display shows
    double acc = 0;
    int op = -1;              ///< pending operator code 12..15, -1 none
    bool fresh = true;        ///< next digit starts a new entry
    bool error = false;
    void Press(int code);
    std::vector<int> Codes() const; ///< display glyphs, most significant first (max 11)
};

// Decoded native art belongs to one asset reader/session. Keep it across LCD
// frames without retaining another game's pixels or permanent failures globally.
class SpriteCache {
public:
    struct Impl;
    SpriteCache();
    ~SpriteCache();
    Impl& Data();
private:
    std::unique_ptr<Impl> impl;
};

/// v = {app, colour, args...}. Returns RGBA8 640x480 or nullopt for bad keys.
///   DigitalWatch / AnalogWatch: hour, minute
///   Calculator: operator code, glyph codes...
///   Pedometer / Counter: value
///   PokemonList: 6 x (species, form, gender, hp permille, item, valid), touched slot, hop offset
///   Friendship: 6 x (species, form, gender, hearts 0-2, valid), touched slot, hop offset
///   CoinToss: face (0 heads, 1 tails), animation frame (-1 resting, 0-11 flipping)
///   MemoPad: tool (0 pencil, 1 eraser), revision
///   Roulette: revision, arrow angle (quantized 30 degree frames)
///   KitchenTimer: minutes, seconds, running, alarm frame (-1 none)
///   Dowsing: ring radius, centre x/y, marker count, then marker x/y pairs
///   EggMonitor: 2 x (species, form, gender, valid), egg present, touched slot, hop offset
///   History: 12 x (species, form) pairs, oldest first, touched slot, hop offset
///   MarkingMap: cursor x, y (-1 none), roamer 1 x, y, roamer 2 x, y (-1 none), blink phase 0-2,
///               hidden-part bits, then 6 marker x, y (0, 0 = tray)
///   DotArtist: revision (dots from the DotArtSurface store: 768 values 0-3)
///   ChainCounter: current species, current count, then 3 x (species, count)
///   HiddenMoves: 8 states (0 not owned, 1 gray, 2 black)
///   Calendar: month, today, weekday of the 1st (0 Sunday), days in month, mark bits
///   ColorChanger: (uses colour)
/// scale1000 > 1000 renders the display larger (nearest, with the dot grid drawn at the new size):
/// module:lp:poketchx/<scale1000>/<app>/<colour>/<args...>.
/// `lang`: the UI suffix of the text art (the timer buttons, the Hidden Moves names).
std::optional<lp_unity::Image> Render(lp_unity::Reader& unity, const std::vector<int>& v, int scale1000 = 1000,
                                      std::string_view lang = "en", SpriteCache* cache = nullptr, const lp_unity::Image* nursery_title = nullptr);

/// The Roulette arrow alone (88x88, transparent around it) in the display colour, for a
/// rotating overlay: module:lp:pktarrow/<colour>.
std::optional<lp_unity::Image> RenderArrow(lp_unity::Reader& unity, int colour, int scale1000 = 1000,
                                            SpriteCache* cache = nullptr);
/// Dowsing overlays: a 400x400 sonar ring (10 px, transparent around it) and a 40x40 found-item
/// dot, in the display colour: module:lp:pktring/<colour>, module:lp:pktdot/<colour>.
std::optional<lp_unity::Image> RenderRing(int colour, bool dot, int scale1000 = 1000);

/// Key for Render, "module:lp:poketch/..." (ints joined by '/').
std::string Key(const std::vector<int>& v);

} // namespace lp_poketch
