// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
// D4B150B29A931CD381E2A30318FC299F2B0EE36D native MSG data reader.
// Independently derived from EDCEA0/EDECD0/EC0D60/EBFD20 and live snapshots.
// Caller supplies bool read(uint64_t, void*, size_t). No guest calls/writes.
//
// Control codes (native dispatch EBFD20 -> [main+220AD38]): byte0 = 0xF0 | (length/2),
// byte1 = table<<5 | function. Every handler was traced in the D4B1 image. Arguments are
// 2-byte pairs after the header:
// value = (lo-1) | (hi==0xFF ? 0 : (hi-1)<<8). Text-producing handlers append through
// EB9FD0/EBA490 in draw modes 0/5; the reader reproduces only the ones below from the
// game's own data. Anything else still fails the whole text closed; nothing is guessed.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "core/mods/modules/dsmod_module_sdk.h"
namespace p5r_dialogue {
struct Choice {
    std::string text;
    uint16_t source_index{};
};
// First reason a text could not be reproduced (research instrumentation and tests).
enum class Reason : uint8_t { None, Control, Glyph, Byte, Substitution };
struct Fallback {
    Reason reason{Reason::None};
    uint16_t code{};
    uint8_t size{};
    std::array<uint8_t, 32> bytes{};
};
struct Snapshot {
    bool active{}, choice_active{}, ready{}, unsupported{}, torn{};
    uint64_t context{};
    int selected{-1};
    uint16_t message{0xffff}, page{0xffff};
    std::string speaker, text;
    std::vector<Choice> choices;
    Fallback fallback;
#ifdef P5R_DIALOGUE_TRACE
    std::vector<uint8_t> raw; // research builds: displayed page bytes for offline re-decoding
#endif
};
// Absolute guest addresses resolved by the caller from D4B1 accessor instructions.
// Zero means "not resolved": every substitution needing it falls back.
struct Globals {
    uint64_t order_language_slot{}; // CE1A10: u32 at [slot] (1 swaps the name order)
    uint64_t text_language_slot{};  // CE4880: u32 at [slot]
    uint64_t name_a{}, name_b{}, full_name{}, name_language{}; // 880B10 / 880BA0 / 880A80
    uint64_t thieves{}, thieves_language{};                    // 881070
    std::array<uint64_t, 8> item_tables{}; // 88BFA0 categories 0..7: {char* base, u16* offsets}
    std::array<uint64_t, 3> unit_tables{}; // 89BD50 / 89BD70 / 89BD90
    uint64_t stat_names_slot{}, stat_levels_slot{}; // 869970 / 869820
};
using dsmod_sdk::Le16;
using dsmod_sdk::Le32;
using dsmod_sdk::Le64;

// EN FONT0 glyph index -> UTF-8. Index = (hi-0x80)*128 + (lo-0x80) (native EBC5F4). Each
// entry was read off the atlas decoded from EN/FONT/FONT0.FNT; unlisted glyphs (mostly
// Japanese) fail closed.
struct GlyphEntry {
    uint16_t index;
    const char* utf8;
};
inline constexpr GlyphEntry kGlyphs[] = {
    {266, "\xe3\x83\xbb"}, {280, "\xe3\x80\x87"}, {285, "\xe2\x80\xa6"},  {302, "\xc3\x97"},
    {316, "\xc2\xa5"},     {320, "\xe2\x98\x86"}, {321, "\xe2\x98\x85"},  {322, "\xe2\x97\x8b"},
    {323, "\xe2\x97\x8f"}, {324, "\xe2\x97\x8e"}, {325, "\xe2\x97\x87"},  {326, "\xe2\x97\x86"},
    {327, "\xe2\x96\xa1"}, {328, "\xe2\x96\xa0"}, {329, "\xe2\x96\xb3"},  {330, "\xe2\x96\xb2"},
    {331, "\xe2\x96\xbd"}, {332, "\xe2\x96\xbc"}, {333, "\xe2\x80\xbb"},  {334, "\xe3\x80\x92"},
    {335, "\xe2\x86\x92"}, {336, "\xe2\x86\x90"}, {337, "\xe2\x86\x91"},  {338, "\xe2\x86\x93"},
    {339, "\xe3\x80\x93"}, {465, "\xe2\x80\x93"}, {466, "\xe2\x80\x94"},  {468, "\xe2\x84\xa2"},
    {502, "\xc3\x80"},     {503, "\xc3\x81"},     {504, "\xc3\x82"},      {505, "\xc3\x83"},
    {506, "\xc3\x84"},     {507, "\xc3\x85"},     {508, "\xc3\x86"},      {509, "\xc3\x87"},
    {510, "\xc3\x88"},     {511, "\xc3\x89"},     {512, "\xc3\x8a"},      {513, "\xc3\x8b"},
    {514, "\xc3\x8c"},     {515, "\xc3\x8d"},     {516, "\xc3\x8e"},      {517, "\xc3\x8f"},
    {518, "\xc3\x90"},     {519, "\xc3\x91"},     {520, "\xc3\x92"},      {521, "\xc3\x93"},
    {522, "\xc3\x94"},     {523, "\xc3\x95"},     {524, "\xc3\x96"},      {525, "\xc3\x98"},
    {526, "\xc3\x99"},     {527, "\xc3\x9a"},     {528, "\xc3\x9b"},      {529, "\xc3\x9c"},
    {530, "\xc3\x9d"},     {531, "\xc3\x9e"},     {532, "\xc3\x9f"},      {533, "\xc3\xa0"},
    {534, "\xc3\xa1"},     {535, "\xc3\xa2"},     {536, "\xc3\xa3"},      {537, "\xc3\xa4"},
    {538, "\xc3\xa5"},     {539, "\xc3\xa6"},     {540, "\xc3\xa7"},      {541, "\xc3\xa8"},
    {542, "\xc3\xa9"},     {543, "\xc3\xaa"},     {544, "\xc3\xab"},      {545, "\xc3\xac"},
    {546, "\xc3\xad"},     {547, "\xc3\xae"},     {548, "\xc3\xaf"},      {549, "\xc3\xb0"},
    {550, "\xc3\xb1"},     {551, "\xc3\xb2"},     {552, "\xc3\xb3"},      {553, "\xc3\xb4"},
    {554, "\xc3\xb5"},     {555, "\xc3\xb6"},     {556, "\xc3\xb8"},      {557, "\xc3\xb9"},
    {558, "\xc3\xba"},     {559, "\xc3\xbb"},     {560, "\xc3\xbc"},      {561, "\xc3\xbd"},
    {562, "\xc3\xbe"},     {563, "\xc3\xbf"},     {3062, "\xe6\x9e\x9a"}, {3619, "\xe2\x99\xaa"},
};
inline bool EnGlyph(uint16_t code, std::string& out) {
    const unsigned hi = code >> 8, lo = code & 0xff;
    if (hi < 0x80 || lo < 0x80)
        return false;
    const unsigned index = (hi - 0x80) * 128 + (lo - 0x80);
    if (index < 95) {
        out.push_back(char(0x20 + index));
        return true;
    } // ASCII duplicates, atlas 0..94
    const auto* end = std::end(kGlyphs);
    const auto* it = std::lower_bound(std::begin(kGlyphs), end, index,
                                      [](const GlyphEntry& e, unsigned v) { return e.index < v; });
    if (it == end || it->index != index)
        return false;
    out += it->utf8;
    return true;
}
// Kept for compatibility: callers can pass their own validated map instead.
struct NoGlyphMap {
    bool operator()(uint16_t, std::string&) const {
        return false;
    }
};
struct EnGlyphMap {
    bool operator()(uint16_t c, std::string& o) const {
        return EnGlyph(c, o);
    }
};

enum class Ctl : uint8_t {
    Unsupported,
    Skip,
    End,
    Var,
    VarItem,
    Item,
    NameA,
    NameB,
    NameFull,
    Thieves,
    StatName,
    StatLevel,
    Icon,
    Unit
};
// Classification by byte1 (the native dispatch key). Skip = handler appends nothing in any
// draw mode and returns 0 (colour/style, waits, voice, bustup, camera, sound, effects).
inline Ctl Classify(uint8_t b1) {
    switch (b1) {
    case 0x01:
    case 0x02:
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x22:
    case 0x23:
    case 0x26:
    case 0x27:
    case 0x28:
    case 0x2B:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x46:
    case 0x47:
    case 0x61:
    case 0x85:
    case 0x86:
    case 0x87:
    case 0x88:
    case 0x89:
    case 0x8A:
    case 0x8E:
    case 0x8F:
    case 0x90:
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97:
    case 0x98:
    case 0x99:
    case 0x9A:
    case 0x9B:
    case 0x9D:
    case 0xA2:
    case 0xA3:
    case 0xA8:
    case 0xA9:
    case 0xAA:
    case 0xAB:
    case 0xAC:
    case 0xAD:
    case 0xAE:
    case 0xAF:
    case 0xB0:
    case 0xB1:
    case 0xB2:
    case 0xB3:
        return Ctl::Skip;
    case 0x21:
    case 0x24:
        return Ctl::End; // both handlers return 1: page ends
    case 0x44:
        return Ctl::Var; // EB6730
    case 0x45:
        return Ctl::VarItem; // EB67E0
    case 0x84:
        return Ctl::Item; // EB7520
    case 0x81:
        return Ctl::NameA; // EB72F0
    case 0x82:
        return Ctl::NameB; // EB7390
    case 0x83:
        return Ctl::NameFull; // EB7430
    case 0x9C:
        return Ctl::Thieves; // EB8B10
    case 0x8C:
        return Ctl::StatName; // EB7AB0
    case 0x8D:
        return Ctl::StatLevel; // EB7B50
    case 0x8B:
        return Ctl::Icon; // EB79E0: social-stat icon glyph only
    case 0x9F:
        return Ctl::Unit; // EB8E00
    default:
        return Ctl::Unsupported;
    }
}
// Native argument decoding (e.g. EB6774..EB678C). Argument i starts at control+2+2i.
inline bool Arg(const uint8_t* c, size_t len, unsigned i, uint16_t& v) {
    const size_t at = 2 + 2 * size_t(i);
    if (at + 1 >= len)
        return false;
    const uint8_t lo = c[at], hi = c[at + 1];
    v = uint16_t(uint8_t(lo - 1)) | (hi == 0xff ? 0 : uint16_t(uint8_t(hi - 1)) << 8);
    return true;
}

template <class Read, class Glyph>
class Expander {
public:
    Expander(Read& r, uint64_t context, const Globals* g, Glyph glyph, Fallback& fb)
        : read(r), ctx(context), globals(g), glyph_map(glyph), fallback(fb) {}
    bool vars_read{};
    std::array<uint8_t, 0x100> vars{};
    // The protagonist's display name as the game substitutes it for unit 1, short variant
    // (EB8E00 -> 894030/894090): name_a unless the language's name order swaps it.
    bool ProtagonistName(std::string& out) {
        uint32_t order{};
        out.clear();
        if (!Lang(order))
            return false;
        const bool first = order != 1;
        return Name(first ? globals->name_a : globals->name_b, 0x14, 1, out);
    }
    // Decode one NUL/F121-terminated text. depth>0: substituted string (no further expansion).
    bool Text(const uint8_t* p, size_t n, std::string& out, int depth = 0) {
        if (!depth)
            out.clear();
        for (size_t i = 0; i < n;) {
            const uint8_t c = p[i];
            if (!c)
                return Finish(out, depth);
            if (c >= 0xf0) {
                const size_t len = (c & 15) * 2;
                if (len < 2 || len > n - i)
                    return Fail(Reason::Control, p + i, std::min<size_t>(n - i, 2), out);
                const Ctl kind = Classify(p[i + 1]);
                if (kind == Ctl::End)
                    return Finish(out, depth);
                if (kind == Ctl::Skip) {
                    i += len;
                    continue;
                }
                if (depth || kind == Ctl::Unsupported)
                    return Fail(Reason::Control, p + i, len, out);
                if (!Expand(kind, p + i, len, out)) {
                    if (fallback.reason == Reason::None)
                        Record(Reason::Substitution, p + i, len);
                    out.clear();
                    return false;
                }
                if (out.size() > 1024)
                    return Fail(Reason::Substitution, p + i, len, out);
                i += len;
                continue;
            }
            if (c >= 0x80) {
                if (i + 1 >= n || !glyph_map(uint16_t(c) << 8 | p[i + 1], out))
                    return Fail(Reason::Glyph, p + i, std::min<size_t>(n - i, 2), out);
                if (out.size() > 1024)
                    return Fail(Reason::Glyph, p + i, 2, out);
                i += 2;
                continue;
            }
            if (c != '\n' && (c < 0x20 || c == 0x7f))
                return Fail(Reason::Byte, p + i, 1, out);
            out.push_back(char(c));
            if (out.size() > 1024)
                return Fail(Reason::Byte, p + i, 1, out);
            ++i;
        }
        return Fail(Reason::Byte, p, 0, out); // no terminator within bounded input
    }
    // Native high-bit speaker ID: context+198 string table (EDD248).
    bool VarText(uint16_t idx, std::string& out, bool& present) {
        uint64_t ptr{};
        present = false;
        if (!Var(idx, ptr))
            return false;
        if (!ptr)
            return true;
        present = true;
        return Nested(ptr, 256, out);
    }

private:
    Read& read;
    uint64_t ctx;
    const Globals* globals;
    Glyph glyph_map;
    Fallback& fallback;
    static bool Finish(std::string& out, int depth) {
        if (!depth)
            while (!out.empty() && out.back() == '\n')
                out.pop_back();
        return true;
    }
    void Record(Reason r, const uint8_t* p, size_t n) {
        fallback.reason = r;
        fallback.code = n >= 2 ? uint16_t(p[0]) << 8 | p[1] : (n ? p[0] : 0);
        fallback.size = uint8_t(std::min<size_t>(n, fallback.bytes.size()));
        std::copy(p, p + fallback.size, fallback.bytes.begin());
    }
    bool Fail(Reason r, const uint8_t* p, size_t n, std::string& out) {
        if (fallback.reason == Reason::None)
            Record(r, p, n);
        out.clear();
        return false;
    }
    template <class T>
    bool Get(uint64_t at, T& v) {
        uint8_t b[sizeof(T)];
        if (at < 0x10000 || !read(at, b, sizeof b))
            return false;
        std::memcpy(&v, b, sizeof v);
        return true;
    }
    bool Var(uint16_t idx, uint64_t& ptr) {
        if (idx >= 32)
            return false; // EE15B0 installs exactly 32 slots
        if (!vars_read) {
            if (ctx > std::numeric_limits<uint64_t>::max() - 0x298 ||
                !read(ctx + 0x198, vars.data(), vars.size()))
                return false;
            vars_read = true;
        }
        ptr = Le64(vars.data() + 8 * idx);
        return true;
    }
    // Bounded NUL-terminated guest string, decoded without further substitution.
    bool Nested(uint64_t at, size_t max, std::string& out) {
        std::array<uint8_t, 256> b{};
        if (!max || max > b.size() || at < 0x10000)
            return false;
        for (size_t n = 0; n < max;) {
            const size_t chunk = std::min<size_t>(16, max - n);
            if (!read(at + n, b.data() + n, chunk)) { // near the end of a mapping: single bytes
                if (!read(at + n, b.data() + n, 1))
                    return false;
                if (!b[n])
                    return Text(b.data(), n + 1, out, 1);
                ++n;
                continue;
            }
            const auto* z = std::find(b.data() + n, b.data() + n + chunk, uint8_t(0));
            if (z != b.data() + n + chunk)
                return Text(b.data(), size_t(z - b.data()) + 1, out, 1);
            n += chunk;
        }
        return false; // no terminator within the native field size
    }
    bool Lang(uint32_t& order) {
        uint64_t slot{};
        return globals && Get(globals->order_language_slot, slot) && Get(slot, order) && order != 3;
    }
    // Name buffers are valid when their stored language is unset, current, or both < 7
    // (880B34..880B5C); otherwise the game converts them (8A6CE0) and we fail closed.
    bool NameLanguageOk(uint64_t tag_at) {
        uint64_t slot{};
        uint32_t cur{};
        int8_t tag{};
        if (!globals || !Get(tag_at, tag))
            return false;
        if (tag == -1)
            return true;
        if (!Get(globals->text_language_slot, slot) || !Get(slot, cur))
            return false;
        return int64_t(cur) == tag || (uint8_t(tag) <= 6 && cur < 7);
    }
    bool Name(uint64_t buf, size_t max, size_t min_len, std::string& out) {
        if (!buf || !NameLanguageOk(globals->name_language))
            return false;
        std::array<uint8_t, 0x28> b{};
        if (max > b.size() || !read(buf, b.data(), max))
            return false;
        const auto nul = std::find(b.begin(), b.begin() + max, 0);
        if (nul == b.begin() + max || size_t(nul - b.begin()) < min_len)
            return false; // default name path
        return Text(b.data(), max, out, 1);
    }
    bool Table(uint64_t table, uint16_t id, std::string& out) {
        uint64_t base{}, offsets{};
        uint16_t off{};
        return table && Get(table, base) && Get(table + 8, offsets) && base && offsets &&
               Get(offsets + 2ull * id, off) && Nested(base + off, 64, out);
    }
    bool ItemName(uint32_t id, std::string& out) { // 88BFA0 dispatch on id>>12
        const unsigned cat = (id >> 12) & 15;
        if (!globals || cat >= globals->item_tables.size())
            return false; // 8: language path; 9+: invalid
        return Table(globals->item_tables[cat], uint16_t(id & 0xfff), out);
    }
    bool Ftd(uint64_t slot, uint64_t& data) {
        uint64_t t{};
        uint32_t off{};
        if (!globals || !Get(slot, t) || !Get(t + 0x10, off))
            return false;
        data = t + off;
        return true;
    }
    bool Expand(Ctl kind, const uint8_t* c, size_t len, std::string& out) {
        uint32_t order{};
        if (!Lang(order))
            return false;
        uint16_t a0{}, a1{}, a2{};
        switch (kind) {
        case Ctl::Var: { // EB6730 -> EBA490
            uint64_t ptr{};
            if (!Arg(c, len, 0, a0) || !Var(a0, ptr))
                return false;
            return !ptr || Nested(ptr, 256, out);
        }
        case Ctl::VarItem: { // EB67E0
            uint64_t ptr{};
            if (!Arg(c, len, 0, a0) || !Arg(c, len, 1, a1) || !Arg(c, len, 2, a2))
                return false;
            if (a2 > 0x1f)
                return true; // native appends nothing
            if (!Var(a2, ptr))
                return false;
            if (!ptr)
                return true;
            if (a1 == 9999 || a1 == 0xffff) {
                uint32_t id{};
                return Get(ptr, id) && ItemName(id, out);
            } // icon omitted
            return Nested(ptr, 256, out);
        }
        case Ctl::Item: // EB7520: icon omitted, name kept
            return Arg(c, len, 2, a2) && ItemName(a2, out);
        case Ctl::NameA:
            return Name(globals->name_a, 0x14, 1, out);
        case Ctl::NameB:
            return Name(globals->name_b, 0x14, 1, out);
        case Ctl::NameFull: { // EB7430: 894030(1) ' ' 894090(1)
            const bool swap = order == 1;
            if (!Name(swap ? globals->name_b : globals->name_a, 0x14, 1, out))
                return false;
            out.push_back(' ');
            return Name(swap ? globals->name_a : globals->name_b, 0x14, 1, out);
        }
        case Ctl::Thieves: { // 881070
            if (!globals->thieves || !NameLanguageOk(globals->thieves_language))
                return false;
            return Nested(globals->thieves, 0x1c, out);
        }
        case Ctl::StatName: { // EB7AB0 -> 869970
            uint64_t data{};
            uint32_t count{};
            if (!Arg(c, len, 0, a0))
                return false;
            if (a0 > 4)
                return true;
            if (!Ftd(globals->stat_names_slot, data) || !Get(data + 8, count))
                return false;
            return count <= a0 || Nested(data + 0x10 + 20ull * a0, 20, out);
        }
        case Ctl::StatLevel: { // EB7B50 -> 869820
            uint64_t data{};
            if (!Arg(c, len, 0, a0) || !Arg(c, len, 1, a1))
                return false;
            if (a0 > 4 || uint16_t(a1 - 1) > 4)
                return true;
            return Ftd(globals->stat_levels_slot, data) &&
                   Nested(data + 100ull * a0 + 20ull * a1 - 4, 20, out);
        }
        case Ctl::Icon:
            return true;  // stat icon glyph, no text
        case Ctl::Unit: { // EB8E00 -> 893FF0/894030/894090
            if (!Arg(c, len, 0, a0) || !Arg(c, len, 1, a1) || a1 > 2 || a0 == 10)
                return false; // 10: flag 0x849
            if (a0 == 1) {
                if (a1 == 0)
                    return Name(globals->full_name, 0x28, 2, out);
                const bool first = (a1 == 1) != (order == 1);
                return Name(first ? globals->name_a : globals->name_b, 0x14, 1, out);
            }
            if (a0 >= 64)
                return false;
            return Table(globals->unit_tables[a1], a0, out);
        }
        default:
            return false;
        }
    }
};

// Decode text bytes without guest access (substitutions fall back).
template <class Glyph = EnGlyphMap>
bool Decode(const uint8_t* p, size_t n, std::string& out, Glyph glyph = {},
            Fallback* why = nullptr) {
    Fallback local;
    auto none = [](uint64_t, void*, size_t) { return false; };
    Expander<decltype(none), Glyph> e(none, 0, nullptr, glyph, why ? *why : local);
    return e.Text(p, n, out);
}

// Protagonist display name (unit 1, short variant) from the live name buffers; false when the
// buffers are unresolved, unset (default-name path) or in a language the game would convert.
template <class Read, class Glyph = EnGlyphMap>
bool ProtagonistName(Read read, const Globals* globals, std::string& out, Glyph glyph = {}) {
    Fallback fb{};
    Expander<Read, Glyph> ex(read, 0, globals, glyph, fb);
    return globals && ex.ProtagonistName(out) && !out.empty();
}
template <class Read, class Glyph = EnGlyphMap>
Snapshot ReadContext(Read read, uint64_t context, const Globals* globals = nullptr,
                     Glyph glyph = {}) {
    Snapshot s;
    s.context = context;
    std::array<uint8_t, 0x90> h{};
    if (context < 0x10000 || !read(context, h.data(), h.size()))
        return s;
    const auto q = [&](size_t o) { return Le64(h.data() + o); };
    s.choice_active = int16_t(Le16(h.data() + 0x80)) > 0 && q(0x70) != 0;
    const bool message_active = int8_t(h[0x40]) > 0 && q(0x38) != 0;
    s.active = message_active || s.choice_active;
    if (!s.active)
        return s;
    s.message = Le16(h.data() + 0x48);
    s.page = Le16(h.data() + 0x4a);
    const uint64_t bmd = q(8);
    std::array<uint8_t, 0x20> bh{};
    if (bmd < 0x10000 || !read(bmd, bh.data(), bh.size()))
        return s;
    const uint32_t size = Le32(bh.data() + 4), count = Le32(bh.data() + 0x18);
    if (bmd > std::numeric_limits<uint64_t>::max() - size)
        return s;
    if (Le32(bh.data() + 8) != 0x3147534d || size < 0x20 || size > 32 * 1024 * 1024 ||
        count > 65535 || 0x20ull + 8ull * count + 16 > size)
        return s;
    auto inside = [&](uint64_t a, size_t n) {
        return a >= bmd && a - bmd < size && n <= size - (a - bmd);
    };
    auto br = [&](uint64_t a, void* p, size_t n) { return inside(a, n) && read(a, p, n); };
    auto u32 = [&](uint64_t a, uint32_t& v) {
        uint8_t b[4];
        if (!br(a, b, 4))
            return false;
        v = Le32(b);
        return true;
    };
    auto rel = [&](uint64_t a, uint64_t& dst) {
        uint32_t raw;
        if (!u32(a, raw) || !raw)
            return false;
        const int64_t d = int32_t(raw);
        if (d < 0) {
            if (a < uint64_t(-d))
                return false;
            dst = a - uint64_t(-d);
        } else {
            if (a > std::numeric_limits<uint64_t>::max() - uint64_t(d))
                return false;
            dst = a + uint64_t(d);
        }
        return inside(dst, 1);
    };
    Expander<Read, Glyph> ex(read, context, globals, glyph, s.fallback);
    auto text = [&](uint64_t a, std::string& out) {
        if (!inside(a, 1))
            return false;
        std::array<uint8_t, 2048> b{};
        size_t n = std::min<size_t>(b.size(), size - (a - bmd));
        if (!br(a, b.data(), n))
            return false;
        if (!ex.Text(b.data(), n, out)) {
            s.unsupported = true;
            return false;
        }
        return true;
    };
    auto record = [&](uint32_t id, uint32_t kind, uint64_t& r) {
        if (id >= count)
            return false;
        uint32_t k;
        uint64_t e = bmd + 0x20 + id * 8ull;
        return u32(e, k) && k == kind && rel(e + 4, r);
    };
    // +30 remains the displayed normal message while a choice is on top.
    bool ok = true;
    const uint64_t rec = q(0x30);
    if (message_active && (!rec || !q(0x38)))
        return s;
    if (rec && q(0x38)) {
        std::array<uint8_t, 0x1c> rh{};
        uint64_t expected{};
        if (!record(Le16(h.data() + 0x48), 0, expected) || expected != rec ||
            !br(rec, rh.data(), rh.size()))
            return s;
        const uint16_t pages = Le16(rh.data() + 0x18), speaker = Le16(rh.data() + 0x1a);
        uint16_t page = Le16(h.data() + 0x4a);
        if (!pages || pages > 256 || !inside(rec + 0x1c, pages * 4ull))
            return s;
        // Completion increments the page counter; the final page stays behind choices.
        if (s.choice_active && page == pages)
            page = pages - 1;
        if (page >= pages)
            return s;
        uint64_t ptr{};
        if (!rel(rec + 0x1c + page * 4ull, ptr))
            return s;
        ok = text(ptr, s.text) && ok;
#ifdef P5R_DIALOGUE_TRACE
        if (inside(ptr, 1)) {
            s.raw.resize(std::min<size_t>(384, size - (ptr - bmd)));
            if (!br(ptr, s.raw.data(), s.raw.size()))
                s.raw.clear();
        }
#endif
        if (speaker != 0xffff) {
            if (speaker & 0x8000) { // EDD248: context+198 string table; null pointer = no name
                bool present{};
                if (!ex.VarText(speaker & 0x7fff, s.speaker, present)) {
                    if (s.fallback.reason == Reason::None) {
                        s.fallback.reason = Reason::Substitution;
                        s.fallback.code = speaker;
                    }
                    s.unsupported = true;
                    ok = false;
                    s.speaker.clear();
                }
            } else {
                const uint64_t meta = bmd + 0x20 + count * 8ull;
                uint64_t table{};
                uint32_t names{};
                if (!u32(meta + 4, names) || speaker >= names || names > 1024 ||
                    !rel(meta, table) || !inside(table, names * 4ull) ||
                    !rel(table + speaker * 4ull, ptr))
                    return s;
                ok = text(ptr, s.speaker) && ok;
            }
        }
    }
    if (s.choice_active) {
        uint64_t cr{};
        if (!record(Le32(h.data() + 0x68), 1, cr))
            return s;
        uint8_t ch[0x20];
        if (!br(cr, ch, sizeof ch))
            return s;
        const uint16_t choices = Le16(ch + 0x1a);
        const uint32_t mask = Le32(h.data() + 0x78);
        if (!choices || choices > 32 || !inside(cr + 0x20, choices * 4ull))
            return s;
        s.selected = int16_t(Le16(h.data() + 0x82));
        for (uint16_t i = 0; i < choices; ++i) {
            if (mask & (1u << i))
                continue;
            if (s.choices.size() == 8)
                return s;
            uint64_t p{};
            if (!rel(cr + 0x20 + i * 4ull, p))
                return s;
            Choice c;
            c.source_index = i;
            ok = text(p, c.text) && ok;
            s.choices.push_back(std::move(c));
        }
        if (s.choices.size() != Le16(h.data() + 0x86) || s.selected < 0 ||
            size_t(s.selected) >= s.choices.size() ||
            s.choices[s.selected].source_index != Le16(h.data() + 0x84))
            return s;
    }
    // Guard against changed records, pages, choice lifecycle/selection, variables and cleanup.
    std::array<uint8_t, 0x90> after{};
    auto torn = [&] {
        s.torn = true;
        s.text.clear();
        s.speaker.clear();
        s.choices.clear();
        return s;
    };
    if (!read(context, after.data(), after.size()))
        return s;
    for (auto range : {std::pair<size_t, size_t>{8, 8}, {0x20, 8}, {0x30, 0x1e}, {0x68, 0x20}})
        if (std::memcmp(h.data() + range.first, after.data() + range.first, range.second))
            return torn();
    if (ex.vars_read) {
        std::array<uint8_t, 0x100> v{};
        if (!read(context + 0x198, v.data(), v.size()) || v != ex.vars)
            return torn();
    }
    s.ready = ok;
    return s;
}
} // namespace p5r_dialogue
