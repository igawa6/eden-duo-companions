// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// TEST-ONLY reference (not compiled into the module): the music-title table earlier module
// versions embedded, reduced to cue / row / inherited and an FNV-1a-64 hash of each title text. The
// shipped module builds the table at run time from the game's romfs (p5r_game_text.h ->
// p5r_assets::BuildMusicTitles); the romfs tests check that result against this copy row by row.
#pragma once
#include <cstdint>

#include "core/mods/modules/dsmod_module_sdk.h"

namespace p5r_bgm_ref {
using namespace dsmod_sdk::int_types;
// Titles = what the game's own Music Player shows. Row R of EN/INIT/MYPTABLE.BIN
// mypSoundDataTable (u16 BE cue) is drawn with sprite R of EN/MYPALACE/SOUND/MUSIC_TITLE_001.SPD.
// Rows 0..77: sprite text == mypSoundNameTable text. Rows 78..106: the name table only holds dev
// placeholders, so the displayed sprite text is the title (art only; the module publishes no
// text for them). Rows with an empty sprite (83 84 85 86 90 91 99) are in no Music Player list
// (mypSoundSortTable_*): no title. Cue 958 has two rows: 85 (placeholder, empty sprite, unlisted)
// and 105 (listed in STORY) -> row 105; its name-table text is a placeholder that belongs to
// cue 957 (sprite 87).
// row = Thieves Den data row (bgm.index). inherited = 1: the cue has no row of its own but
// BGM.ACB resolves it (Cue -> Sequence -> Track -> Synth -> OutsideLink -> Waveform) to the SAME
// waveform as the titled cue(s), all of one title.
struct Title {
    u16 cue;
    u8 row;
    u8 inherited;
    std::uint64_t text_fnv; // FNV-1a-64 of the title's UTF-8 text (the text itself is not kept)
};
inline constexpr Title kTitles[] = {
    {0, 12, 0, 0xf59a698734935109ULL},    {1, 39, 1, 0x24a31e20b1f1edb4ULL},
    {4, 26, 1, 0x5b604ec76c079a7dULL},    {6, 20, 0, 0x83dbbea6a44fb492ULL},
    {8, 21, 0, 0x1555f0f711e9d12bULL},    {9, 22, 0, 0xb901b8b3c2d37ffeULL},
    {10, 5, 1, 0x2153b6a1d68d08edULL},    {11, 6, 0, 0x2e338315b25f075aULL},
    {12, 30, 0, 0x8357eb0a34464307ULL},   {13, 58, 1, 0x000b84c0fca6ec4bULL},
    {14, 14, 0, 0xe17fa4e22cbcfbe2ULL},   {15, 15, 0, 0xd5f0078e6447d013ULL},
    {16, 27, 1, 0x84b7b308cf83560fULL},   {17, 65, 1, 0x37751fa27e1a4b1eULL},
    {18, 19, 0, 0x6b9dd7d9fa34b7c4ULL},   {19, 4, 0, 0x5208576f2a09042fULL},
    {20, 32, 0, 0xa2783e8b877edb93ULL},   {22, 18, 0, 0x21d8ca54f0953bb4ULL},
    {23, 9, 0, 0xe9f1be98f7831ac7ULL},    {24, 10, 0, 0xffdc17e13317443cULL},
    {25, 5, 0, 0x2153b6a1d68d08edULL},    {28, 11, 0, 0xc7b40f30d3e2d996ULL},
    {29, 27, 0, 0x84b7b308cf83560fULL},   {30, 13, 0, 0x18f58923283351ddULL},
    {31, 16, 0, 0xe6260584aa7fb550ULL},   {32, 7, 0, 0xa2021b9bacbdf185ULL},
    {33, 24, 0, 0xf81826a6cb4f8cb2ULL},   {34, 24, 1, 0xf81826a6cb4f8cb2ULL},
    {35, 31, 0, 0xe682f802f82197d5ULL},   {36, 29, 0, 0x7da66f8f776fac64ULL},
    {37, 23, 0, 0x20cff730ed6e4fd4ULL},   {38, 28, 0, 0x7a216adeac06ca5fULL},
    {39, 17, 0, 0x49854dea5c139b7eULL},   {101, 2, 0, 0x438ffe9733df9ec2ULL},
    {102, 76, 1, 0x9dd91618b1957e3eULL},  {207, 52, 0, 0xd4f7ae210080f6a1ULL},
    {208, 48, 0, 0xc8a19446b6a1074cULL},  {209, 46, 0, 0x4ccc2db78264434eULL},
    {210, 47, 0, 0x706f37874b0e5749ULL},  {212, 45, 0, 0xbc8cb03ecd7572efULL},
    {213, 49, 0, 0x8beb385cca75782cULL},  {214, 50, 0, 0x523cf37f7e1d3157ULL},
    {215, 43, 0, 0xb984c0ce03945f70ULL},  {216, 44, 0, 0xf19df4ee05a0df1dULL},
    {217, 51, 0, 0x56b27d56c2a1fa60ULL},  {300, 53, 0, 0x04b3d1e53e15e3e8ULL},
    {310, 53, 1, 0x04b3d1e53e15e3e8ULL},  {320, 53, 1, 0x04b3d1e53e15e3e8ULL},
    {330, 57, 0, 0xd9bcfb693091558cULL},  {331, 58, 0, 0x000b84c0fca6ec4bULL},
    {332, 59, 0, 0xdf4a7c0c6306427fULL},  {333, 61, 0, 0x63d93eda1feef708ULL},
    {334, 62, 0, 0xc65bd926a818f06aULL},  {335, 56, 0, 0xea80a42f15d8c108ULL},
    {336, 60, 0, 0x6078b1c222346487ULL},  {340, 54, 0, 0x56061aa1301f2bacULL},
    {341, 55, 0, 0x03c95cfb08fae04fULL},  {400, 64, 0, 0x75e1977b7c65e7e3ULL},
    {401, 64, 1, 0x75e1977b7c65e7e3ULL},  {402, 64, 1, 0x75e1977b7c65e7e3ULL},
    {403, 65, 0, 0x37751fa27e1a4b1eULL},  {410, 66, 0, 0x04932ca54bd8887eULL},
    {411, 67, 0, 0x70ecd33540efb097ULL},  {420, 68, 1, 0xeed76e8afe9f604aULL},
    {421, 68, 0, 0xeed76e8afe9f604aULL},  {422, 69, 0, 0x874292571aba84f3ULL},
    {430, 70, 0, 0x9c158bec4ace7f4fULL},  {431, 70, 1, 0x9c158bec4ace7f4fULL},
    {432, 71, 0, 0xcf6d3187d088412aULL},  {433, 71, 1, 0xcf6d3187d088412aULL},
    {440, 72, 0, 0x812cf72cafb2c307ULL},  {441, 73, 0, 0xa43a996fcde3c782ULL},
    {450, 74, 0, 0x9409090487fdf977ULL},  {451, 74, 1, 0x9409090487fdf977ULL},
    {460, 75, 0, 0xf9f8da19a044f2e3ULL},  {461, 75, 1, 0xf9f8da19a044f2e3ULL},
    {470, 76, 0, 0x9dd91618b1957e3eULL},  {471, 3, 0, 0x2a41fb7bc53648fbULL},
    {480, 77, 0, 0x81af8945e17e8d8dULL},  {495, 63, 0, 0x92d756b6da2d6714ULL},
    {500, 42, 0, 0x3c1c01d8e3461735ULL},  {501, 42, 1, 0x3c1c01d8e3461735ULL},
    {502, 40, 0, 0x1d4a5713b49551b2ULL},  {503, 41, 0, 0x182846c1905bf786ULL},
    {600, 35, 0, 0x7e4e4ca98f0914ddULL},  {610, 38, 0, 0xc481858d1e74dbe1ULL},
    {620, 39, 0, 0x24a31e20b1f1edb4ULL},  {630, 8, 0, 0xfda792742a9f912bULL},
    {640, 34, 0, 0x0068e9104de61815ULL},  {641, 37, 0, 0xa4e84846e6345a67ULL},
    {642, 36, 0, 0x6e6fef1cd3827b93ULL},  {700, 25, 0, 0x4975778763889ecdULL},
    {720, 26, 0, 0x5b604ec76c079a7dULL},  {721, 26, 1, 0x5b604ec76c079a7dULL},
    {800, 1, 0, 0x4215390140206f67ULL},   {801, 0, 0, 0x1d3bd7003bbd7d5fULL},
    {802, 33, 0, 0x749b2e338c470d4cULL},  {901, 79, 0, 0x90e9adc407c8ddabULL},
    {904, 80, 0, 0x928590140f431070ULL},  {905, 97, 0, 0x36223eb83ac923f6ULL},
    {906, 98, 0, 0xbaadc0402b37db89ULL},  {907, 94, 0, 0x728fb62339f1de50ULL},
    {908, 95, 0, 0x8df11027014558b3ULL},  {909, 96, 0, 0x0f01444d9ebce8e2ULL},
    {913, 81, 0, 0xa7572dd2f229fdc4ULL},  {915, 82, 0, 0xc305b4e23acef708ULL},
    {923, 92, 0, 0xfd1ab74f54c1ccffULL},  {924, 93, 0, 0x1f38e970f634ab4aULL},
    {931, 88, 0, 0xb64c55c68e72a139ULL},  {933, 100, 0, 0x2b5b2a5e92627a16ULL},
    {934, 101, 0, 0x62e69b70f8118cf8ULL}, {935, 102, 0, 0xd7e2d2b956e8a25dULL},
    {936, 103, 0, 0x57685e5caaf2ba3cULL}, {939, 89, 0, 0xb831680447ca5767ULL},
    {940, 106, 0, 0x91eb614f0047dc20ULL}, {956, 78, 0, 0xff6a42eacab660e4ULL},
    {957, 87, 0, 0x2622652ce92e87e5ULL},  {958, 105, 0, 0xe0ee3353520af1bfULL},
    {960, 104, 0, 0xbd944fdc78c6ba28ULL},
};

inline const Title* LookupRef(std::int64_t cue) {
    if (cue < 0 || cue > 0xffff)
        return nullptr;
    unsigned lo = 0, hi = sizeof(kTitles) / sizeof(kTitles[0]);
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (kTitles[mid].cue < cue)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < sizeof(kTitles) / sizeof(kTitles[0]) && kTitles[lo].cue == cue ? &kTitles[lo]
                                                                               : nullptr;
}
} // namespace p5r_bgm_ref
