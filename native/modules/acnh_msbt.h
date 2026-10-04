// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// MSBT (MsgStdBn) message files for the ACNH companion: Message/<Group>_<Lang>.sarc.zs members
// "<path>.msbt" (e.g. String_USen.sarc.zs -> "Item/STR_ItemName_00_Ftr.msbt"). Sections LBL1
// (hashed label buckets -> text index), TXT2 (offsets -> UTF-16 strings; ACNH uses encoding 1).
// Control tags: 0x0E group type argsize(bytes) args... and 0x0F group type (closing tag).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace acnh {

class Msbt {
public:
    bool Parse(const std::vector<uint8_t>& bytes);
    /// The label's string with control tags kept (UTF-16 code units, no terminator).
    const std::u16string* Raw(std::string_view label) const;
    /// UTF-8 with control tags removed ("" when the label is missing).
    std::string Text(std::string_view label) const;
    bool Has(std::string_view label) const {
        return raw.contains(std::string{label});
    }
    const std::unordered_map<std::string, std::u16string>& All() const {
        return raw;
    }

    /// UTF-16 (tags kept) -> UTF-8 with the control tags removed.
    static std::string ToUtf8(std::u16string_view s);

private:
    std::unordered_map<std::string, std::u16string> raw;
};

// ---- text encoding helpers (every UTF conversion of the module) ----
/// UTF-8 encode one code point.
void AppendUtf8(std::string& out, char32_t cp);
/// Plain UTF-16 -> UTF-8 (no tag handling; a lone surrogate becomes U+FFFD).
std::string Utf16ToUtf8(std::u16string_view s);
/// Next code point of a UTF-8 string (invalid bytes -> U+FFFD), advancing `i`.
char32_t NextCodepoint(std::string_view s, size_t& i);

} // namespace acnh
