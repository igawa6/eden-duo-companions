// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Wonder game-data catalog: BYML + MSBT readers and the course/world/area tables built from them.
// Every read is bounds-checked against its buffer; anything out of range makes the whole parse
// fail (nullopt) instead of producing partial data.

#include "wonder_catalog.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>

namespace WonderCatalog {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;

// Endian-aware bounded reader. Out-of-range reads set `bad` and return 0.
struct Cursor {
    std::span<const u8> s;
    bool be{};
    bool bad{};

    bool Has(std::size_t o, std::size_t n) {
        if (o > s.size() || n > s.size() - o) {
            bad = true;
            return false;
        }
        return true;
    }
    u8 U8(std::size_t o) {
        return Has(o, 1) ? s[o] : 0;
    }
    u16 U16(std::size_t o) {
        if (!Has(o, 2))
            return 0;
        return be ? static_cast<u16>(s[o] << 8 | s[o + 1])
                  : static_cast<u16>(s[o] | s[o + 1] << 8);
    }
    u32 U32(std::size_t o) {
        if (!Has(o, 4))
            return 0;
        u32 v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<u32>(s[o + i]) << (be ? 8 * (3 - i) : 8 * i);
        return v;
    }
    u64 U64(std::size_t o) {
        const u64 a = U32(o), b = U32(o + 4);
        return be ? (a << 32 | b) : (b << 32 | a);
    }
};

// ------------------------------------------------------------------ BYML

struct BymlParser {
    Cursor c;
    BymlLimits limits;
    std::size_t nodes{};
    std::vector<std::string> keys;
    std::vector<std::string> strings;

    // Node header: type byte + u24 count, laid out in the file's byte order.
    bool Header(std::size_t off, u8& type, u32& count) {
        const u32 w = c.U32(off);
        if (c.bad)
            return false;
        if (c.be) {
            type = static_cast<u8>(w >> 24);
            count = w & 0xFFFFFF;
        } else {
            type = static_cast<u8>(w & 0xFF);
            count = w >> 8;
        }
        return true;
    }

    bool StringTable(u32 off, std::vector<std::string>& out) {
        if (off == 0)
            return true;
        u8 type;
        u32 count;
        if (!Header(off, type, count) || type != 0xC2)
            return false;
        if (!c.Has(off + 4, (static_cast<std::size_t>(count) + 1) * 4))
            return false;
        out.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            const std::size_t at = off + static_cast<std::size_t>(c.U32(off + 4 + i * 4));
            if (at >= c.s.size())
                return false;
            const void* end = std::memchr(c.s.data() + at, 0, c.s.size() - at);
            if (!end)
                return false;
            out.emplace_back(reinterpret_cast<const char*>(c.s.data() + at),
                             static_cast<const u8*>(end) - (c.s.data() + at));
        }
        return true;
    }

    bool Value(u8 type, u32 raw, std::size_t depth, BymlNode& out) {
        if (++nodes > limits.max_nodes)
            return false;
        using T = BymlNode::Type;
        switch (type) {
        case 0xA0:
            if (raw >= strings.size())
                return false;
            out.type = T::String;
            out.str = strings[raw];
            return true;
        case 0xA1:
        case 0xA2: {
            const u32 size = c.U32(raw);
            const std::size_t data = raw + (type == 0xA2 ? 8u : 4u);
            if (c.bad || !c.Has(data, size))
                return false;
            out.type = T::Binary;
            out.str.assign(reinterpret_cast<const char*>(c.s.data() + data), size);
            return true;
        }
        case 0xD0:
            out.type = T::Bool;
            out.num.b = raw != 0;
            return true;
        case 0xD1:
            out.type = T::Int;
            out.num.i = static_cast<std::int32_t>(raw);
            return true;
        case 0xD2:
            out.type = T::Float;
            out.num.f = std::bit_cast<float>(raw);
            return true;
        case 0xD3:
            out.type = T::UInt;
            out.num.u = raw;
            return true;
        case 0xD4:
        case 0xD5:
        case 0xD6: {
            const u64 v = c.U64(raw);
            if (c.bad)
                return false;
            out.type = type == 0xD4 ? T::Int64 : type == 0xD5 ? T::UInt64 : T::Double;
            if (type == 0xD4)
                out.num.i = static_cast<s64>(v);
            else if (type == 0xD5)
                out.num.u = v;
            else
                out.num.f = std::bit_cast<double>(v);
            return true;
        }
        case 0xFF:
            out.type = T::Null;
            return true;
        case 0xC0:
        case 0xC1:
        case 0x20:
        case 0x21:
            return Container(raw, type, depth + 1, out);
        default:
            return false; // unknown type: fail closed
        }
    }

    bool Container(std::size_t off, u8 expect, std::size_t depth, BymlNode& out) {
        if (depth > limits.max_depth || (off & 3) != 0)
            return false;
        u8 type;
        u32 count;
        if (!Header(off, type, count) || type != expect)
            return false;
        if (count > limits.max_nodes - std::min(nodes, limits.max_nodes))
            return false;
        using T = BymlNode::Type;
        if (type == 0xC0) {
            const std::size_t values = (off + 4 + count + 3) & ~std::size_t{3};
            if (!c.Has(off + 4, count) || !c.Has(values, static_cast<std::size_t>(count) * 4))
                return false;
            out.type = T::Array;
            out.array.resize(count);
            for (u32 i = 0; i < count; ++i)
                if (!Value(c.s[off + 4 + i], c.U32(values + i * 4), depth, out.array[i]))
                    return false;
            return true;
        }
        if (type == 0xC1) {
            if (!c.Has(off + 4, static_cast<std::size_t>(count) * 8))
                return false;
            out.type = T::Dict;
            out.dict.resize(count);
            for (u32 i = 0; i < count; ++i) {
                const std::size_t e = off + 4 + static_cast<std::size_t>(i) * 8;
                const u32 w = c.U32(e);
                const u32 key = c.be ? w >> 8 : w & 0xFFFFFF;
                const u8 vtype = static_cast<u8>(c.be ? w & 0xFF : w >> 24);
                if (key >= keys.size())
                    return false;
                out.dict[i].key = keys[key];
                if (!Value(vtype, c.U32(e + 4), depth, out.dict[i].value))
                    return false;
            }
            return true;
        }
        // 0x20 / 0x21: hash-keyed dicts (BYML v7): count x {hash (u32|u64), value u32}, then
        // count type bytes. Keys are kept as "0x<hash>".
        const std::size_t hash_size = type == 0x20 ? 4 : 8;
        const std::size_t entry = hash_size + 4;
        const std::size_t types = off + 4 + entry * count;
        if (!c.Has(off + 4, entry * count) || !c.Has(types, count))
            return false;
        out.type = T::Dict;
        out.dict.resize(count);
        for (u32 i = 0; i < count; ++i) {
            const std::size_t e = off + 4 + entry * i;
            const u64 hash = hash_size == 4 ? c.U32(e) : c.U64(e);
            char buf[24];
            const int n = std::snprintf(buf, sizeof(buf), "0x%0*llx", static_cast<int>(hash_size * 2),
                                        static_cast<unsigned long long>(hash));
            out.dict[i].key.assign(buf, n > 0 ? static_cast<std::size_t>(n) : 0);
            if (!Value(c.s[types + i], c.U32(e + hash_size), depth, out.dict[i].value))
                return false;
        }
        return true;
    }
};

std::string BaseName(std::string_view path) {
    const auto slash = path.find_last_of('/');
    if (slash != std::string_view::npos)
        path.remove_prefix(slash + 1);
    return std::string(path.substr(0, path.find('.')));
}

// "Work/Stage/StageParam/X.game__stage__StageParam.gyml" -> "/Stage/StageParam/X....bgyml"
std::optional<std::string> GymlToRomfs(std::string_view path) {
    if (!path.starts_with("Work/") || !path.ends_with(".gyml") || path.find("..") != path.npos)
        return std::nullopt;
    path.remove_prefix(4);
    path.remove_suffix(5);
    return std::string(path) + ".bgyml";
}

// Number after `prefix` up to the end or a '_' ("Course20" -> 20, "Course850_Course" -> 850).
std::optional<int> NumberAfter(std::string_view s, std::string_view prefix) {
    if (!s.starts_with(prefix))
        return std::nullopt;
    s.remove_prefix(prefix.size());
    const auto end = s.find('_');
    const std::string_view digits = s.substr(0, end);
    if (digits.empty() || digits.size() > 6)
        return std::nullopt;
    int v = 0;
    const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), v);
    if (r.ec != std::errc{} || r.ptr != digits.data() + digits.size() || v < 0)
        return std::nullopt;
    return v;
}

// ------------------------------------------------------------------ MSBT

void AppendUtf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | cp >> 6);
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | cp >> 12);
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | cp >> 18);
        out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::optional<std::string> MsbtText(Cursor& c, std::size_t begin, std::size_t end) {
    std::string out;
    std::size_t p = begin;
    while (p + 2 <= end) {
        u32 ch = c.U16(p);
        p += 2;
        if (ch == 0)
            break;
        if (ch == 0x0E) { // tag: group, type, param bytes, params
            if (p + 6 > end)
                return std::nullopt;
            const u16 group = c.U16(p), type = c.U16(p + 2), size = c.U16(p + 4);
            p += 6;
            if (size > end - p)
                return std::nullopt;
            p += size;
            if (group == 0 && type == 4)
                out += '\n';
            continue;
        }
        if (ch == 0x0F) { // end tag: group, type
            if (p + 4 > end)
                return std::nullopt;
            p += 4;
            continue;
        }
        if (ch >= 0xD800 && ch < 0xDC00 && p + 2 <= end) {
            const u32 lo = c.U16(p);
            if (lo >= 0xDC00 && lo < 0xE000) {
                p += 2;
                ch = 0x10000 + ((ch - 0xD800) << 10) + (lo - 0xDC00);
            } else {
                ch = 0xFFFD;
            }
        } else if (ch >= 0xD800 && ch < 0xE000) {
            ch = 0xFFFD;
        }
        AppendUtf8(out, ch);
    }
    if (c.bad)
        return std::nullopt;
    return out;
}

std::string StripWs(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n'))
        s.pop_back();
    std::size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\n'))
        ++b;
    return s.substr(b);
}

std::optional<std::string> PathField(const BymlNode* n) {
    if (!n)
        return std::nullopt;
    const auto s = n->String();
    return s ? GymlToRomfs(*s) : std::nullopt;
}

// Message archive: /Mals/<lang>.Product.<ver>.sarc.zs; the version suffix changes per update
// (100 for 1.0.0, 120 for 1.2.x). Known ones first, then a fixed descending sweep.
std::optional<std::vector<u8>> LoadMessages(const WonderAssets::RomfsReader& read,
                                            std::string_view lang) {
    std::vector<int> versions{120, 110, 100};
    for (int v = 199; v >= 100; --v)
        if (std::find(versions.begin(), versions.end(), v) == versions.end())
            versions.push_back(v);
    for (const int v : versions) {
        const auto raw = read("/Mals/" + std::string(lang) + ".Product." + std::to_string(v) +
                              ".sarc.zs");
        if (raw)
            return WonderAssets::Zstd(*raw);
    }
    return std::nullopt;
}

std::optional<std::map<std::string, std::string>> LoadMsbt(std::span<const u8> sarc,
                                                           std::string_view name) {
    const auto m = WonderAssets::SarcMember(sarc, name);
    return m ? ParseMsbt(*m) : std::nullopt;
}

} // namespace

// ------------------------------------------------------------------ BymlNode

const BymlNode* BymlNode::Get(std::string_view key) const {
    if (type != Type::Dict)
        return nullptr;
    for (const auto& e : dict)
        if (e.key == key)
            return &e.value;
    return nullptr;
}
const BymlNode* BymlNode::At(std::size_t index) const {
    return type == Type::Array && index < array.size() ? &array[index] : nullptr;
}
std::size_t BymlNode::Size() const {
    return type == Type::Array ? array.size() : type == Type::Dict ? dict.size() : 0;
}
std::optional<std::string_view> BymlNode::String() const {
    if (type != Type::String)
        return std::nullopt;
    return std::string_view(str);
}
std::optional<std::int64_t> BymlNode::Integer() const {
    switch (type) {
    case Type::Int:
    case Type::Int64:
        return num.i;
    case Type::UInt:
    case Type::UInt64:
        if (num.u > static_cast<u64>(std::numeric_limits<s64>::max()))
            return std::nullopt;
        return static_cast<s64>(num.u);
    default:
        return std::nullopt;
    }
}
std::optional<double> BymlNode::Number() const {
    switch (type) {
    case Type::Float:
    case Type::Double:
        return num.f;
    case Type::Int:
    case Type::Int64:
        return static_cast<double>(num.i);
    case Type::UInt:
    case Type::UInt64:
        return static_cast<double>(num.u);
    default:
        return std::nullopt;
    }
}

// ------------------------------------------------------------------ parsers

std::optional<BymlNode> ParseByml(std::span<const u8> data, BymlLimits limits) {
    std::optional<std::vector<u8>> unzipped;
    if (data.size() >= 4 && data[0] == 0x28 && data[1] == 0xB5 && data[2] == 0x2F &&
        data[3] == 0xFD) {
        unzipped = WonderAssets::Zstd(data);
        if (!unzipped)
            return std::nullopt;
        data = *unzipped;
    }
    if (data.size() < 16)
        return std::nullopt;
    BymlParser p;
    p.c = Cursor{data, false, false};
    p.limits = limits;
    if (data[0] == 'Y' && data[1] == 'B')
        p.c.be = false;
    else if (data[0] == 'B' && data[1] == 'Y')
        p.c.be = true;
    else
        return std::nullopt;
    const u16 version = p.c.U16(2);
    if (version < 2 || version > 7)
        return std::nullopt;
    const u32 key_table = p.c.U32(4), string_table = p.c.U32(8), root = p.c.U32(12);
    if (!p.StringTable(key_table, p.keys) || !p.StringTable(string_table, p.strings))
        return std::nullopt;
    BymlNode out;
    if (root == 0)
        return out; // empty document
    u8 type;
    u32 count;
    if (!p.Header(root, type, count))
        return std::nullopt;
    if (type != 0xC0 && type != 0xC1 && type != 0x20 && type != 0x21)
        return std::nullopt;
    if (!p.Container(root, type, 0, out) || p.c.bad)
        return std::nullopt;
    return out;
}

std::optional<BymlNode> LoadByml(const WonderAssets::RomfsReader& read, const std::string& path) {
    const auto raw = read(path);
    if (!raw)
        return std::nullopt;
    return ParseByml(*raw);
}

std::optional<std::map<std::string, std::string>> ParseMsbt(std::span<const u8> data) {
    if (data.size() < 0x20 || std::memcmp(data.data(), "MsgStdBn", 8) != 0)
        return std::nullopt;
    Cursor c{data, false, false};
    if (data[8] == 0xFE && data[9] == 0xFF)
        c.be = true;
    else if (!(data[8] == 0xFF && data[9] == 0xFE))
        return std::nullopt;
    if (data[0x0C] != 1) // 1 = UTF-16; UTF-8/UTF-32 archives are not used by this game
        return std::nullopt;
    const u16 sections = c.U16(0x0E);
    if (sections > 64)
        return std::nullopt;

    std::vector<std::pair<std::string, u32>> labels;
    std::vector<std::string> texts;
    bool have_labels = false, have_texts = false;
    std::size_t o = 0x20;
    for (u16 i = 0; i < sections; ++i) {
        if (!c.Has(o, 16))
            return std::nullopt;
        const std::string_view magic(reinterpret_cast<const char*>(data.data() + o), 4);
        const u32 size = c.U32(o + 4);
        const std::size_t b = o + 16;
        if (!c.Has(b, size))
            return std::nullopt;
        const std::size_t end = b + size;
        if (magic == "LBL1") {
            have_labels = true;
            const u32 buckets = c.U32(b);
            if (buckets > 0x10000 || !c.Has(b + 4, static_cast<std::size_t>(buckets) * 8))
                return std::nullopt;
            for (u32 k = 0; k < buckets; ++k) {
                const u32 n = c.U32(b + 4 + k * 8);
                std::size_t p = b + c.U32(b + 8 + k * 8);
                // Each label takes >= 5 bytes, so a well-formed block holds at most size / 5.
                if (n > size || labels.size() + n > size / 5)
                    return std::nullopt;
                for (u32 j = 0; j < n; ++j) {
                    if (p >= end)
                        return std::nullopt;
                    const u8 len = data[p];
                    if (p + 1 + len + 4 > end)
                        return std::nullopt;
                    labels.emplace_back(
                        std::string(reinterpret_cast<const char*>(data.data() + p + 1), len),
                        c.U32(p + 1 + len));
                    p += 5 + len;
                }
            }
        } else if (magic == "TXT2") {
            have_texts = true;
            const u32 n = c.U32(b);
            if (n > size / 4 || !c.Has(b + 4, static_cast<std::size_t>(n) * 4))
                return std::nullopt;
            texts.reserve(n);
            for (u32 k = 0; k < n; ++k) {
                const std::size_t from = b + c.U32(b + 4 + k * 4);
                const std::size_t to = k + 1 < n ? b + c.U32(b + 8 + k * 4) : end;
                if (from > to || to > end)
                    return std::nullopt;
                auto t = MsbtText(c, from, to);
                if (!t)
                    return std::nullopt;
                texts.push_back(std::move(*t));
            }
        }
        o = (end + 15) & ~std::size_t{15};
    }
    if (!have_labels || !have_texts || c.bad)
        return std::nullopt;
    std::map<std::string, std::string> out;
    for (auto& [name, index] : labels) {
        if (index >= texts.size())
            return std::nullopt;
        out[std::move(name)] = texts[index];
    }
    return out;
}

// ------------------------------------------------------------------ catalog

std::optional<int> Catalog::CourseAt(int world, int key) const {
    const auto it = world_course.find({world, key});
    if (it == world_course.end())
        return std::nullopt;
    return it->second;
}

std::optional<Catalog> BuildCatalog(const WonderAssets::RomfsReader& read, std::string_view lang,
                                    bool details) {
    if (!read || lang.empty() || lang.size() > 8 ||
        lang.find_first_of("/.\\") != std::string_view::npos)
        return std::nullopt;
    Catalog cat;

    const auto msg_sarc = LoadMessages(read, lang);
    std::map<std::string, std::string> course_msgs, course_msgs_lf, world_msgs;
    if (msg_sarc) {
        if (auto m = LoadMsbt(*msg_sarc, "GameMsg/Name_CourseRemoveLineFeed.msbt"))
            course_msgs = std::move(*m);
        if (auto m = LoadMsbt(*msg_sarc, "GameMsg/Name_Course.msbt"))
            course_msgs_lf = std::move(*m);
        if (auto m = LoadMsbt(*msg_sarc, "GameMsg/Name_World.msbt"))
            world_msgs = std::move(*m);
    }

    // Worlds: WorldList order gives the world id (1-based), as the game's world number.
    std::vector<std::pair<int, std::string>> world_info; // (world id, WorldMapInfo path)
    if (const auto list = LoadByml(read, "/Stage/WorldList/WorldList.game__stage__WorldList.bgyml")) {
        if (const auto* paths = list->Get("WorldMapStagePath")) {
            for (std::size_t i = 0; i < paths->Size() && i < 64; ++i) {
                const auto stage = PathField(paths->At(i));
                if (!stage)
                    continue;
                const int world = static_cast<int>(i) + 1;
                std::optional<std::string> info;
                if (const auto param = LoadByml(read, *stage))
                    if (const auto* comp = param->Get("Components"))
                        info = PathField(comp->Get("WorldMapInfo"));
                if (!info)
                    info = "/Stage/WorldMapInfo/" + BaseName(*stage) +
                           ".game__stage__WorldMapInfo.bgyml";
                world_info.emplace_back(world, *info);
            }
        }
    }
    if (world_info.empty())
        return std::nullopt;

    std::set<std::pair<int, std::string>> courses; // (course id, "CourseNNN_Course")
    for (const auto& [world, path] : world_info) {
        const auto info = LoadByml(read, path);
        if (!info)
            continue;
        if (const auto* no = info->Get("WorldNo_"); no && no->String())
            cat.world_internal[world] = std::string(*no->String());
        if (const auto* label = info->Get("WorldNameLabel"); label && label->String()) {
            const std::string l(*label->String());
            if (const auto it = world_msgs.find(l); it != world_msgs.end())
                cat.world_title[world] = StripWs(it->second);
            std::string plain;
            if (l.starts_with("WorldName"))
                if (const auto it = world_msgs.find("WorldNameOrigin" + l.substr(9));
                    it != world_msgs.end())
                    plain = StripWs(it->second);
            if (plain.empty() && cat.world_title.contains(world))
                plain = cat.world_title[world];
            if (!plain.empty())
                cat.world_name[world] = plain;
        }
        const auto* table = info->Get("CourseTable");
        if (!table)
            continue;
        for (std::size_t i = 0; i < table->Size(); ++i) {
            const auto* row = table->At(i);
            const auto* key = row ? row->Get("Key") : nullptr;
            const auto* stage = row ? row->Get("StagePath") : nullptr;
            if (!key || !stage || !key->String() || !stage->String())
                continue;
            const auto k = NumberAfter(*key->String(), "Course");
            const std::string name = BaseName(*stage->String());
            const auto id = NumberAfter(name, "Course");
            if (!k || !id || !name.ends_with("_Course"))
                continue;
            cat.world_course[{world, *k}] = *id;
            courses.emplace(*id, name);
        }
    }

    for (const auto& [id, name] : courses) {
        // Course name: StageParam -> CourseInfo -> CourseNameLabel -> message.
        std::optional<std::string> info_path;
        if (const auto param =
                LoadByml(read, "/Stage/StageParam/" + name + ".game__stage__StageParam.bgyml"))
            if (const auto* comp = param->Get("Components"))
                info_path = PathField(comp->Get("CourseInfo"));
        if (!info_path)
            info_path = "/Stage/CourseInfo/" + name + ".game__stage__CourseInfo.bgyml";
        if (const auto info = LoadByml(read, *info_path)) {
            if (const auto* label = info->Get("CourseNameLabel"); label && label->String()) {
                const std::string l(*label->String());
                std::string text;
                if (const auto it = course_msgs.find(l); it != course_msgs.end())
                    text = it->second;
                else if (const auto it2 = course_msgs_lf.find(l); it2 != course_msgs_lf.end())
                    text = it2->second;
                std::replace(text.begin(), text.end(), '\n', ' ');
                text = StripWs(std::move(text));
                if (!text.empty())
                    cat.course_name[id] = std::move(text);
            }
        }
        // Areas: the course map unit's RefStages, index = area index.
        const auto unit = LoadByml(read, "/BancMapUnit/" + name + ".bcett.byml.zs");
        const auto* refs = unit ? unit->Get("RefStages") : nullptr;
        if (!refs)
            continue;
        for (std::size_t i = 0; i < refs->Size() && i <= 0x7E; ++i) {
            const auto* ref = refs->At(i);
            if (!ref || !ref->String())
                continue;
            const std::string res = BaseName(*ref->String());
            if (res.empty())
                continue;
            const int area = static_cast<int>(i);
            cat.area_resource[{id, area}] = res;
            if (!details)
                continue;
            if (const auto path = GymlToRomfs(*ref->String()))
                if (const auto param = LoadByml(read, *path)) {
                    const auto* cat_node = param->Get("Category");
                    const auto* label = param->Get("Label");
                    if (cat_node && cat_node->String() && *cat_node->String() == "Area" && label &&
                        label->String())
                        cat.area_label[{id, area}] = std::string(*label->String());
                }
        }
    }
    if (!cat.Ready())
        return std::nullopt;
    return cat;
}

// ------------------------------------------------------------------ routes

float Route::Progress(float x, float y) const {
    if (path.size() < 2 || !(length > 0.0f))
        return 0.0f;
    float best_d2 = std::numeric_limits<float>::max(), best_s = 0.0f, walked = 0.0f;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const RoutePoint& a = path[i];
        const RoutePoint& b = path[i + 1];
        const float vx = b.x - a.x, vy = b.y - a.y;
        const float l2 = vx * vx + vy * vy;
        const float seg = std::sqrt(l2);
        const float t =
            l2 > 1e-6f ? std::clamp(((x - a.x) * vx + (y - a.y) * vy) / l2, 0.0f, 1.0f) : 0.0f;
        const float px = a.x + t * vx - x, py = a.y + t * vy - y;
        const float d2 = px * px + py * py;
        if (d2 < best_d2) {
            best_d2 = d2;
            best_s = walked + t * seg;
        }
        walked += seg;
    }
    const float p = best_s / length;
    return std::isfinite(p) ? std::clamp(p, 0.0f, 1.0f) : 0.0f;
}

namespace {

bool ValidResource(std::string_view resource) {
    return !resource.empty() && resource.size() <= 64 &&
           resource.find_first_of("/.\\") == std::string_view::npos;
}

std::optional<RoutePoint> ActorPoint(const BymlNode& actor) {
    const auto* t = actor.Get("Translate");
    if (!t || t->type != BymlNode::Type::Array || t->Size() < 2)
        return std::nullopt;
    const auto x = t->At(0)->Number(), y = t->At(1)->Number();
    if (!x || !y || !std::isfinite(*x) || !std::isfinite(*y) ||
        std::abs(*x) > std::numeric_limits<float>::max() ||
        std::abs(*y) > std::numeric_limits<float>::max())
        return std::nullopt;
    const RoutePoint p{static_cast<float>(*x), static_cast<float>(*y)};
    return std::isfinite(p.x) && std::isfinite(p.y) ? std::optional{p} : std::nullopt;
}

std::optional<u64> ActorHash(const BymlNode* n) {
    if (!n)
        return std::nullopt;
    if (n->type == BymlNode::Type::UInt || n->type == BymlNode::Type::UInt64)
        return n->num.u;
    if (const auto i = n->Integer(); i && *i >= 0)
        return static_cast<u64>(*i);
    return std::nullopt;
}

std::optional<Route> BuildAreaRoute(const BymlNode& unit,
                                  std::optional<RoutePoint> entrance = std::nullopt,
                                  std::optional<RoutePoint> exit = std::nullopt) {
    const auto* actors = unit.Get("Actors");
    if (!actors || actors->type != BymlNode::Type::Array)
        return std::nullopt;
    Route r;
    std::optional<RoutePoint> start;
    std::vector<std::pair<int, RoutePoint>> goals; // (GoalID, pole) in file order, one per id
    for (std::size_t i = 0; i < actors->Size(); ++i) {
        const auto* a = actors->At(i);
        const auto* g = a ? a->Get("Gyaml") : nullptr;
        if (!g || !g->String())
            continue;
        const auto point = ActorPoint(*a);
        if (!point)
            continue;
        const RoutePoint p = *point;
        const std::string_view gyaml = *g->String();
        const auto* dyn = a->Get("Dynamic");
        auto dyn_int = [&](std::string_view k) -> std::optional<std::int64_t> {
            const auto* n = dyn ? dyn->Get(k) : nullptr;
            return n ? n->Integer() : std::nullopt;
        };
        if (gyaml == "PlayerLocator") {
            if (!start)
                start = p;
            continue;
        }
        if (gyaml == "ObjectGoalPole" || gyaml == "ObjectGoalPoleOnlyPole") {
            const std::int64_t id = dyn_int("GoalID").value_or(0);
            if (id >= 0 && id < 8 &&
                std::none_of(goals.begin(), goals.end(), [&](const auto& g) { return g.first == id; }))
                goals.emplace_back(static_cast<int>(id), p);
            continue;
        }
        RouteMarker::Kind kind{};
        if (gyaml == "RetryPoint")
            kind = RouteMarker::Checkpoint;
        else if (gyaml.find("BigTenLuckyCoin") != gyaml.npos)
            kind = RouteMarker::BigFlowerCoin;
        else if (gyaml.find("WonderFinishWonderSe") != gyaml.npos)
            kind = RouteMarker::WonderSeed;
        else if (gyaml == "ItemWonderFlower" || gyaml == "ItemWonderHole")
            kind = RouteMarker::WonderFlower;
        else
            continue;
        const auto save = dyn_int("SaveId");
        const std::int8_t id =
            save && *save >= 0 && *save <= 127 ? static_cast<std::int8_t>(*save) : -1;
        r.markers.push_back({kind, id, p.x, p.y});
    }
    if (!start)
        start = entrance;
    if (goals.empty() && exit)
        goals.emplace_back(-1, *exit);
    if (!start || goals.empty())
        return std::nullopt;
    r.start = *start;
    const auto dist2 = [](const RoutePoint& a, const RoutePoint& b) {
        const float dx = a.x - b.x, dy = a.y - b.y;
        return dx * dx + dy * dy;
    };
    // The main goal is the pole with the lowest GoalID; a second pole is the secret exit.
    std::sort(goals.begin(), goals.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    r.normal_goal = goals[0].second;
    r.normal_goal_id = goals[0].first;
    if (goals.size() > 1) {
        r.secret_goal = goals[1].second;
        r.secret_goal_id = goals[1].first;
    }
    if (!(dist2(r.normal_goal, r.start) > 1e-4f))
        return std::nullopt;
    std::sort(r.markers.begin(), r.markers.end(), [](const RouteMarker& a, const RouteMarker& b) {
        if (a.x != b.x)
            return a.x < b.x;
        if (a.kind != b.kind)
            return a.kind < b.kind;
        if (a.id != b.id)
            return a.id < b.id;
        return a.y < b.y;
    });
    if (r.markers.size() > 64)
        r.markers.resize(64);
    // The rail ends at the main goal. Its path chains the start through the main route's markers
    // (nearest first) to the main goal, so climbing and zig-zag courses measure progress along the
    // way they are played rather than along one straight line. Markers nearer the secret pole
    // than the main goal belong to the secret route and stay off the path (they are still shown,
    // projected onto it).
    {
        std::vector<RoutePoint> todo;
        for (const auto& m : r.markers) {
            const RoutePoint q{m.x, m.y};
            if (!r.secret_goal || dist2(q, r.normal_goal) <= dist2(q, *r.secret_goal))
                todo.push_back(q);
        }
        std::vector<RoutePoint> path{r.start};
        while (!todo.empty()) {
            const RoutePoint cur = path.back();
            std::size_t best = 0;
            float best_d2 = std::numeric_limits<float>::max();
            for (std::size_t i = 0; i < todo.size(); ++i) {
                const float dx = todo[i].x - cur.x, dy = todo[i].y - cur.y;
                if (dx * dx + dy * dy < best_d2) {
                    best_d2 = dx * dx + dy * dy;
                    best = i;
                }
            }
            path.push_back(todo[best]);
            todo.erase(todo.begin() + static_cast<std::ptrdiff_t>(best));
        }
        path.push_back(r.normal_goal);
        float total = 0.0f;
        for (std::size_t i = 0; i + 1 < path.size(); ++i)
            total += std::hypot(path[i + 1].x - path[i].x, path[i + 1].y - path[i].y);
        r.path = std::move(path);
        r.length = total;
    }
    if (!(r.length > 0.0f) || !std::isfinite(r.length))
        return std::nullopt;
    for (auto& m : r.markers)
        m.progress = r.Progress(m.x, m.y);
    r.normal_progress = r.Progress(r.normal_goal.x, r.normal_goal.y);
    if (r.secret_goal)
        r.secret_progress = r.Progress(r.secret_goal->x, r.secret_goal->y);
    return r;
}

} // namespace

std::optional<Route> BuildRoute(const WonderAssets::RomfsReader& read, std::string_view resource) {
    if (!ValidResource(resource))
        return std::nullopt;
    const auto unit = LoadByml(read, "/BancMapUnit/" + std::string(resource) + ".bcett.byml.zs");
    return unit ? BuildAreaRoute(*unit) : std::nullopt;
}

std::map<std::string, Route> BuildCourseRoutes(const WonderAssets::RomfsReader& read, int course) {
    std::map<std::string, Route> routes;
    if (course < 0 || course > 999)
        return routes;
    char name[32];
    std::snprintf(name, sizeof(name), "Course%03d_Course", course);
    const auto course_unit = LoadByml(read, "/BancMapUnit/" + std::string(name) + ".bcett.byml.zs");
    const auto* refs = course_unit ? course_unit->Get("RefStages") : nullptr;
    if (!refs || refs->type != BymlNode::Type::Array || refs->Size() > 128)
        return routes;
    struct Area {
        std::string resource;
        BymlNode unit;
        std::optional<RoutePoint> spawn;
        bool goal{};
    };
    struct Endpoint {
        std::size_t area;
        RoutePoint point;
    };
    struct Link {
        Endpoint src, dst;
    };
    std::vector<Area> areas;
    std::map<u64, Endpoint> endpoints;
    std::set<u64> duplicate_hashes;
    std::set<std::string> resources;
    for (const auto& ref : refs->array) {
        if (!ref.String())
            continue;
        const std::string resource = BaseName(*ref.String());
        if (!ValidResource(resource) || !resources.insert(resource).second)
            continue;
        auto unit = LoadByml(read, "/BancMapUnit/" + resource + ".bcett.byml.zs");
        const auto* actors = unit ? unit->Get("Actors") : nullptr;
        if (!actors || actors->type != BymlNode::Type::Array)
            continue;
        Area area{resource, {}, {}, false};
        for (const auto& actor : actors->array) {
            const auto* g = actor.Get("Gyaml");
            const auto p = ActorPoint(actor);
            if (!g || !g->String() || !p)
                continue;
            if (*g->String() == "PlayerLocator" && !area.spawn)
                area.spawn = p;
            if (*g->String() == "ObjectGoalPole" || *g->String() == "ObjectGoalPoleOnlyPole") {
                const auto* dyn = actor.Get("Dynamic");
                const auto* id_node = dyn ? dyn->Get("GoalID") : nullptr;
                const auto id = id_node ? id_node->Integer().value_or(0) : 0;
                area.goal |= id >= 0 && id < 8;
            }
            if (const auto hash = ActorHash(actor.Get("Hash")); hash &&
                !endpoints.emplace(*hash, Endpoint{areas.size(), *p}).second)
                duplicate_hashes.insert(*hash);
        }
        area.unit = std::move(*unit);
        areas.push_back(std::move(area));
    }
    for (const auto hash : duplicate_hashes)
        endpoints.erase(hash);
    std::vector<Link> links;
    const auto* raw_links = course_unit->Get("Links");
    if (raw_links && raw_links->type == BymlNode::Type::Array && raw_links->Size() <= 4096) {
        for (const auto& link : raw_links->array) {
            const auto* kind = link.Get("Name");
            const auto src = ActorHash(link.Get("Src")), dst = ActorHash(link.Get("Dst"));
            if (!kind || kind->String() != "NextGoTo" || !src || !dst ||
                !endpoints.contains(*src) || !endpoints.contains(*dst))
                continue;
            const auto a = endpoints.at(*src), b = endpoints.at(*dst);
            if (a.area != b.area)
                links.push_back({a, b});
        }
    }
    // Directed distances rule out dead ends and goal-free loops. RefStages is an index table,
    // not a traversal order: bonus areas commonly return to an earlier entry in that table.
    constexpr int Unreachable = 1000;
    std::vector<int> from_spawn(areas.size(), Unreachable), to_goal(areas.size(), Unreachable);
    for (std::size_t i = 0; i < areas.size(); ++i) {
        if (areas[i].spawn)
            from_spawn[i] = 0;
        if (areas[i].goal)
            to_goal[i] = 0;
    }
    for (std::size_t pass = 0; pass < areas.size(); ++pass)
        for (const auto& link : links) {
            from_spawn[link.dst.area] = std::min(from_spawn[link.dst.area], from_spawn[link.src.area] + 1);
            to_goal[link.src.area] = std::min(to_goal[link.src.area], to_goal[link.dst.area] + 1);
        }
    for (std::size_t i = 0; i < areas.size(); ++i) {
        // Preserve complete legacy rails exactly, even if unrelated course links are broken.
        if (auto r = BuildAreaRoute(areas[i].unit)) {
            routes.emplace(areas[i].resource, std::move(*r));
            continue;
        }
        if (from_spawn[i] == Unreachable || to_goal[i] == Unreachable)
            continue;
        auto entrance = areas[i].spawn;
        if (!entrance) {
            int best = Unreachable;
            for (const auto& link : links)
                if (link.dst.area == i && from_spawn[link.src.area] < best) {
                    best = from_spawn[link.src.area];
                    entrance = link.dst.point;
                }
        }
        if (!entrance)
            continue;
        std::optional<RoutePoint> exit;
        int best = Unreachable;
        float furthest = -1.0f;
        for (const auto& link : links) {
            if (link.src.area != i || to_goal[link.dst.area] >= to_goal[i])
                continue;
            const float distance = std::hypot(link.src.point.x - entrance->x,
                                              link.src.point.y - entrance->y);
            // Prefer the shortest area chain to a pole; same-depth exits use the farthest
            // portal from the entrance so an optional early shortcut does not end the rail.
            if (to_goal[link.dst.area] < best ||
                (to_goal[link.dst.area] == best && distance > furthest)) {
                best = to_goal[link.dst.area];
                furthest = distance;
                exit = link.src.point;
            }
        }
        if (auto r = BuildAreaRoute(areas[i].unit, entrance, exit))
            routes.emplace(areas[i].resource, std::move(*r));
    }
    return routes;
}

} // namespace WonderCatalog
