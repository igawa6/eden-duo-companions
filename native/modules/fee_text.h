// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fire Emblem Engage text: the game's own MSBT ("MsgStdBn") message tables, decoded from the
// player's installed romfs at runtime (never shipped with the companion). Engage stores one MSBT
// blob per TextAsset in Data/StreamingAssets/aa/Switch/fe_assets_message/<region>/<locale>/
// <table>.bytes.bundle; labels are the very keys the game keeps in its data objects
// (MPID_/MGID_ person + Emblem names in "person", MJID_ class names in "job").
//
// Decoding: LBL1 buckets -> TXT2 UTF-16 strings. Control tags are 0x000E group(u16) type(u16)
// size(u16) then `size` argument bytes (the game packs ruby/speaker arguments there); a closing
// tag is 0x000F. Text keeps '\n'; everything else is stripped by SingleLine() when a field must
// fit one line.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fee_text {

struct Table {
    std::unordered_map<std::string, std::string> text; ///< label -> UTF-8 (tags stripped)

    const std::string* Find(std::string_view label) const {
        const auto it = text.find(std::string{label});
        return it == text.end() ? nullptr : &it->second;
    }
    /// "" when the label is missing.
    std::string Get(std::string_view label) const {
        const std::string* t = Find(label);
        return t ? *t : std::string{};
    }
};

/// The player-name tag (group 6 type 3, "[tag g6t3:]": the avatar's player-given name) is kept in
/// decoded text as this byte so Assets::Text can put the name in; every other tag is dropped.
inline constexpr char PlayerNameMark = '\x02';

/// True for a raw message / data key that must never reach the screen: "MPID_x", "M0_x"
/// (^M[A-Z0-9]*_) or "PID_x", "MJID_x" (^[A-Z]{2,5}ID_).
bool IsRawKey(std::string_view text);

/// One MsgStdBn blob -> `out` (cleared first). False on a bad magic/header or no labels.
bool ParseMsbt(const std::uint8_t* data, std::size_t size, Table& out);

/// A display field from decoded text: newlines and runs of whitespace become one space, ends
/// trimmed. Leaves the string empty when there is nothing readable.
std::string SingleLine(std::string_view text);

/// The face sprite name behind a PersonData.UnitIconID: the alphabetic tail after the leading
/// sort digits ("001Lueur" -> "Lueur", "800SoldierMG" -> "SoldierMG"); empty when none.
std::string FaceSprite(std::string_view icon);
/// The avatar's female art family: with gender 2 the hero icons "001Lueur*" / "002Lueur*" become
/// "051Lueur*" / "052Lueur*" (unit dots, palettes); FaceSprite of those is "LueurW" (facethumb,
/// level-up telop and ringbook godface all name the female art "LueurW"). Others unchanged.
std::string GenderedIcon(std::string_view icon, int gender);

/// The help-entry key of a name key: "MPID_Lueur" -> "MPID_H_Lueur", "MJID_X" -> "MJID_H_X".
/// Empty when `key` has no prefix.
std::string HelpKey(std::string_view key);

/// Locale directory ("us/usen", "eu/eude", ...) of an App.Language.Langs value (0..11).
/// "" for an out-of-range value.
const char* LocaleDir(int lang);

} // namespace fee_text
