// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// See smo_romfs_tables.h for the overall contract. Byte-level formats (Yaz0, BYML "YB" v3, MSBT)
// are ported from this project's offline extraction scripts (byml.py, msbt_grep.py), which
// derived them empirically against the game's own romfs. The exact per-table joins below were
// ported from the offline extract_system_data.py script and from hand verification against a
// base-1.0 romfs dump (see this module's test and the PR description for the verification
// trail): which archive, which member, which BYML/MSBT field, in what order.
//
// task-shine-rules.tsv's per-moon achievement weighting/grouping is rebuilt from the
// AchievementInfo dispatch's own native comparison routines -- see ShineStemTable()'s comment
// below for where that came from. Not reproduced here (see smo_romfs_tables.h's top comment for
// why): world-stages.tsv's "category" column (always ""), and one stage/world pairing.

#include "smo_romfs_tables.h"

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "fe3h_ui2d.h"
#include "smo_map.h"
#include "smo_save.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace SmoRomfsTables {
namespace {
using namespace dsmod_sdk::int_types;
using Span = std::span<const u8>;

constexpr size_t kMaxArchive = 64u << 20; // matches smo_map.cpp's ExpandYaz0 bound

// ---------------------------------------------------------------------------------------------
// romfs access
// ---------------------------------------------------------------------------------------------

std::optional<std::vector<u8>> ReadWholeRomfs(const EdenDsmodHostApi& host, const std::string& path) {
    if (!host.read_romfs) return std::nullopt;
    const size_t size = host.read_romfs(host.userdata, path.c_str(), 0, nullptr, 0);
    if (size == 0 || size > kMaxArchive) return std::nullopt;
    std::vector<u8> buf(size);
    if (host.read_romfs(host.userdata, path.c_str(), 0, buf.data(), size) != size) return std::nullopt;
    return buf;
}

// Bounded port of smo_map.cpp's ExpandYaz0 (itself a port of mk8d_assets.cpp's Yaz0 recipe).
// Duplicated rather than shared across translation units/modules, matching this project's
// existing convention (each module file owns its own small format decoders).
bool Fits(Span b, uint64_t p, uint64_t n) { return p <= b.size() && n <= b.size() - p; }
std::optional<std::vector<u8>> ExpandYaz0(Span in) {
    if (in.size() < 16 || std::memcmp(in.data(), "Yaz0", 4) != 0) return std::nullopt;
    const uint32_t size = uint32_t(in[4]) << 24 | uint32_t(in[5]) << 16 | uint32_t(in[6]) << 8 | uint32_t(in[7]);
    if (!size || size > kMaxArchive) return std::nullopt;
    std::vector<u8> out(size);
    size_t si = 16, di = 0;
    uint8_t code = 0, mask = 0;
    while (di < out.size()) {
        if (!mask) {
            if (si >= in.size()) return std::nullopt;
            code = in[si++]; mask = 0x80;
        }
        if (code & mask) {
            if (si >= in.size()) return std::nullopt;
            out[di++] = in[si++];
        } else {
            if (!Fits(in, si, 2)) return std::nullopt;
            const uint8_t a = in[si++], b = in[si++];
            const size_t distance = ((a & 15) << 8) | b;
            if (distance + 1 > di) return std::nullopt;
            size_t count = a >> 4;
            if (!count) {
                if (si >= in.size()) return std::nullopt;
                count = size_t(in[si++]) + 0x12;
            } else {
                count += 2;
            }
            if (count > out.size() - di) return std::nullopt;
            size_t source = di - distance - 1;
            while (count--) out[di++] = out[source++];
        }
        mask >>= 1;
    }
    return out;
}

/// A decoded SARC archive: owns the (possibly Yaz0-expanded) bytes and lists its members.
struct Archive {
    std::vector<u8> bytes;
    std::vector<fe3h_ui2d::SarcFile> files;
};

std::optional<Archive> LoadArchive(const EdenDsmodHostApi& host, const std::string& path) {
    auto raw = ReadWholeRomfs(host, path);
    if (!raw) return std::nullopt;
    Archive out;
    if (raw->size() >= 4 && std::memcmp(raw->data(), "Yaz0", 4) == 0) {
        auto expanded = ExpandYaz0(*raw);
        if (!expanded) return std::nullopt;
        out.bytes = std::move(*expanded);
    } else {
        out.bytes = std::move(*raw);
    }
    out.files = fe3h_ui2d::Sarc(out.bytes);
    if (out.files.empty()) return std::nullopt; // every archive this file reads is a non-empty SARC
    return out;
}

Span MemberBytes(const Archive& a, std::string_view name) {
    for (const auto& f : a.files) {
        if (f.name == name) {
            if (f.offset > a.bytes.size() || f.size > a.bytes.size() - f.offset) return {};
            return Span(a.bytes).subspan(f.offset, f.size);
        }
    }
    return {};
}

std::optional<SmoSave::Node> ParseMember(const Archive& a, std::string_view name) {
    const auto bytes = MemberBytes(a, name);
    if (bytes.empty()) return std::nullopt;
    return SmoSave::ParseByml(bytes);
}

// A targeted "YB" v3 reader for one deep value inside a huge tree. StageData/<Stage>Map.byml
// packs 15 full copies of the level's object placement (one per scenario) -- several million
// BYML nodes for a large kingdom (City: ~8.3M, measured) -- far past SmoSave::ParseByml's
// save-file-sized node budget (2,000,000; raising it for every caller would risk the unbounded-
// memory failure mode that budget exists to prevent). This walker never descends into a hash key
// other than the one requested, so its real cost is the small subtree it decodes (a MapIconList
// array: at most a few dozen small records), not the file's full size. Same node encoding as
// SmoSave::ParseByml (byml.py's module docstring); duplicated rather than shared, matching this
// project's existing per-file decoder convention.
class TargetedByml {
public:
    bool Load(Span data) {
        d_ = data;
        if (!Fits(d_, 0, 0x10) || d_[0] != 'Y' || d_[1] != 'B') return false;
        const uint32_t hash_key_off = dsmod_sdk::Le32(d_.data() + 4);
        const uint32_t str_off = dsmod_sdk::Le32(d_.data() + 8);
        root_off_ = dsmod_sdk::Le32(d_.data() + 0xC);
        return ParseTable(hash_key_off, hash_keys_) && ParseTable(str_off, strings_);
    }

    std::optional<uint32_t> RootArrayCount() const {
        if (!Fits(d_, root_off_, 4)) return std::nullopt;
        const uint32_t v = dsmod_sdk::Le32(d_.data() + root_off_);
        if ((v & 0xFF) != 0xC0) return std::nullopt;
        return v >> 8;
    }

    /// Finds `key` inside the hash at root array index `i` and decodes it fully (bounded, small)
    /// if it is an array or hash. nullopt if the index/key is absent, the key's value is a scalar,
    /// or anything is malformed -- the caller treats a missing key as "no icons this scenario".
    std::optional<SmoSave::Node> FindInRootElement(uint32_t index, std::string_view key) {
        const auto count = RootArrayCount();
        if (!count || index >= *count) return std::nullopt;
        const uint64_t types_off = uint64_t(root_off_) + 4;
        if (!Fits(d_, types_off, *count)) return std::nullopt;
        const uint64_t slots_off = (types_off + *count + 3) & ~uint64_t(3);
        if (!Fits(d_, slots_off, uint64_t(*count) * 4)) return std::nullopt;
        if (d_[types_off + index] != 0xC1) return std::nullopt; // every scenario entry is a hash
        const uint64_t slot = slots_off + 4ull * index;
        if (!Fits(d_, slot, 4)) return std::nullopt;
        const uint32_t elem_off = dsmod_sdk::Le32(d_.data() + slot);
        if (!Fits(d_, elem_off, 4)) return std::nullopt;
        const uint32_t v = dsmod_sdk::Le32(d_.data() + elem_off);
        if ((v & 0xFF) != 0xC1) return std::nullopt;
        const uint32_t hash_count = v >> 8;
        const uint64_t entries_off = uint64_t(elem_off) + 4;
        if (hash_count > 4096 || !Fits(d_, entries_off, uint64_t(hash_count) * 8)) return std::nullopt;
        for (uint32_t e = 0; e < hash_count; ++e) {
            const uint64_t e_off = entries_off + 8ull * e;
            const uint32_t entry = dsmod_sdk::Le32(d_.data() + e_off);
            const uint32_t nidx = entry & 0xFFFFFFu;
            const uint8_t etype = uint8_t(entry >> 24);
            if (nidx >= hash_keys_.size() || hash_keys_[nidx] != key) continue;
            if (etype != 0xC0 && etype != 0xC1) return std::nullopt; // MapIconList is always an array
            const uint64_t value_slot = e_off + 4;
            if (!Fits(d_, value_slot, 4)) return std::nullopt;
            SmoSave::Node out;
            budget_ = 0;
            if (!ParseSmallNode(dsmod_sdk::Le32(d_.data() + value_slot), 0, out)) return std::nullopt;
            return out;
        }
        return std::nullopt;
    }

private:
    Span d_;
    uint32_t root_off_ = 0;
    std::vector<std::string> hash_keys_, strings_;
    uint64_t budget_ = 0;
    static constexpr int kMaxDepth = 16;
    static constexpr uint64_t kMaxNodes = 200000; // generous for one MapIconList subtree

    bool ParseTable(uint32_t off, std::vector<std::string>& out) {
        if (off == 0) return true;
        if (!Fits(d_, off, 4)) return false;
        const uint32_t v = dsmod_sdk::Le32(d_.data() + off);
        const uint32_t count = v >> 8;
        if (count > 65536 || !Fits(d_, uint64_t(off) + 4, uint64_t(count) * 4)) return false;
        out.clear();
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t rel = dsmod_sdk::Le32(d_.data() + off + 4 + 4ull * i);
            const uint64_t start = uint64_t(off) + rel;
            if (!Fits(d_, start, 1)) return false;
            uint64_t end = start;
            while (end < d_.size() && d_[end] != 0) ++end;
            if (end >= d_.size()) return false;
            out.emplace_back(reinterpret_cast<const char*>(d_.data() + start), end - start);
        }
        return true;
    }

    bool ParseSmallNode(uint64_t off, int depth, SmoSave::Node& out) {
        if (depth > kMaxDepth || ++budget_ > kMaxNodes || !Fits(d_, off, 4)) return false;
        const uint32_t v = dsmod_sdk::Le32(d_.data() + off);
        const uint8_t t = uint8_t(v & 0xFF);
        const uint32_t payload = v >> 8;
        switch (t) {
        case 0xA0: // string
            if (payload >= strings_.size()) return false;
            out.type = SmoSave::Node::Type::String; out.s = strings_[payload]; return true;
        case 0xC0: return ParseSmallArray(off + 4, payload, depth, out);
        case 0xC1: return ParseSmallHash(off + 4, payload, depth, out);
        case 0xFF: out.type = SmoSave::Node::Type::Bool; out.b = payload != 0; return true;
        case 0xD2: {
            const uint32_t bits = v & 0xFFFFFF00u;
            float f; std::memcpy(&f, &bits, sizeof f);
            out.type = SmoSave::Node::Type::Float; out.f = f; return true;
        }
        default: out.type = SmoSave::Node::Type::Int; out.i = payload; return true;
        }
    }
    bool ParseSmallScalar(uint8_t t, uint64_t slot, SmoSave::Node& out) {
        if (!Fits(d_, slot, 4)) return false;
        const uint32_t v = dsmod_sdk::Le32(d_.data() + slot);
        switch (t) {
        case 0xA0:
            out.type = SmoSave::Node::Type::String;
            out.s = v < strings_.size() ? strings_[v] : std::string{};
            return true;
        case 0xFF: out.type = SmoSave::Node::Type::Bool; out.b = v != 0; return true;
        case 0xD2: { float f; std::memcpy(&f, &v, sizeof f); out.type = SmoSave::Node::Type::Float; out.f = f; return true; }
        default: out.type = SmoSave::Node::Type::Int; out.i = v; return true;
        }
    }
    bool ParseSmallArray(uint64_t data_off, uint32_t count, int depth, SmoSave::Node& out) {
        if (count > 65536 || !Fits(d_, data_off, count)) return false;
        const uint64_t p = (data_off + count + 3) & ~uint64_t(3);
        if (!Fits(d_, p, uint64_t(count) * 4)) return false;
        out.type = SmoSave::Node::Type::Array;
        out.arr.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t ty = d_[data_off + i];
            const uint64_t slot = p + 4ull * i;
            if (ty == 0xC0 || ty == 0xC1) {
                if (!Fits(d_, slot, 4) || !ParseSmallNode(dsmod_sdk::Le32(d_.data() + slot), depth + 1, out.arr[i]))
                    return false;
            } else if (!ParseSmallScalar(ty, slot, out.arr[i])) return false;
        }
        return true;
    }
    bool ParseSmallHash(uint64_t entries_off, uint32_t count, int depth, SmoSave::Node& out) {
        if (count > 4096 || !Fits(d_, entries_off, uint64_t(count) * 8)) return false;
        out.type = SmoSave::Node::Type::Hash;
        out.hash.clear();
        out.hash.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t e_off = entries_off + 8ull * i;
            const uint32_t e = dsmod_sdk::Le32(d_.data() + e_off);
            const uint32_t nidx = e & 0xFFFFFFu;
            const uint8_t etype = uint8_t(e >> 24);
            const uint64_t slot = e_off + 4;
            std::string key = nidx < hash_keys_.size() ? hash_keys_[nidx] : ("#" + std::to_string(nidx));
            SmoSave::Node val;
            if (etype == 0xC0 || etype == 0xC1) {
                if (!Fits(d_, slot, 4) || !ParseSmallNode(dsmod_sdk::Le32(d_.data() + slot), depth + 1, val))
                    return false;
            } else if (!ParseSmallScalar(etype, slot, val)) return false;
            out.hash.emplace_back(std::move(key), std::move(val));
        }
        return true;
    }
};

// ---------------------------------------------------------------------------------------------
// MSBT ("MsgStdBn"): LBL1 label table -> TXT2 index, UTF-16LE strings. Bounded, fail closed.
// Ported from a verified reference MSBT parser (including its documented 2026-10-20 TXT2-offset
// fix: offsets are relative to the START of the TXT2 body, i.e. to the count
// field itself, not to the end of the offset table).
// ---------------------------------------------------------------------------------------------

constexpr size_t kMaxMsbtLabels = 8192;
constexpr size_t kMaxMsbtStrings = 8192;
constexpr size_t kMaxMsbtStringChars = 4096;

struct Msbt {
    std::unordered_map<std::string, std::u16string> raw;
};

std::optional<Msbt> ParseMsbt(Span b) {
    if (b.size() < 0x20 || std::memcmp(b.data(), "MsgStdBn", 8) != 0) return std::nullopt;
    std::vector<std::u16string> strings;
    std::vector<std::pair<uint32_t, std::string>> labels; // (string index, label)
    size_t sec = 0x20;
    for (int section = 0; section < 8 && sec + 0x10 <= b.size(); ++section) {
        const auto magic = b.subspan(sec, 4);
        const uint32_t size = dsmod_sdk::Le32(b.data() + sec + 4);
        if (!Fits(b, sec + 0x10, size)) return std::nullopt;
        const Span body = b.subspan(sec + 0x10, size);
        if (!std::memcmp(magic.data(), "TXT2", 4)) {
            if (body.size() < 4) return std::nullopt;
            const uint32_t n = dsmod_sdk::Le32(body.data());
            if (n > kMaxMsbtStrings || !Fits(body, 4, uint64_t(n) * 4)) return std::nullopt;
            strings.reserve(n);
            for (uint32_t k = 0; k < n; ++k) {
                const uint32_t off = dsmod_sdk::Le32(body.data() + 4 + 4ull * k);
                if (off > body.size()) return std::nullopt;
                std::u16string s;
                size_t j = off;
                while (j + 1 < body.size() && s.size() < kMaxMsbtStringChars) {
                    const uint16_t c = dsmod_sdk::Le16(body.data() + j);
                    if (c == 0) break;
                    s.push_back(char16_t(c));
                    j += 2;
                }
                strings.push_back(std::move(s));
            }
        } else if (!std::memcmp(magic.data(), "LBL1", 4)) {
            if (body.size() < 4) return std::nullopt;
            const uint32_t n = dsmod_sdk::Le32(body.data());
            if (n > 4096 || !Fits(body, 4, uint64_t(n) * 8)) return std::nullopt;
            for (uint32_t k = 0; k < n; ++k) {
                const uint32_t nlabels = dsmod_sdk::Le32(body.data() + 4 + 8ull * k);
                const uint32_t off = dsmod_sdk::Le32(body.data() + 4 + 8ull * k + 4);
                if (nlabels > 1024 || off > body.size()) return std::nullopt;
                size_t j = off;
                for (uint32_t m = 0; m < nlabels; ++m) {
                    if (j >= body.size() || labels.size() >= kMaxMsbtLabels) return std::nullopt;
                    const uint8_t len = body[j++];
                    if (!Fits(body, j, len) || !Fits(body, j + len, 4)) return std::nullopt;
                    std::string name(reinterpret_cast<const char*>(body.data() + j), len);
                    j += len;
                    const uint32_t idx = dsmod_sdk::Le32(body.data() + j);
                    j += 4;
                    labels.emplace_back(idx, std::move(name));
                }
            }
        }
        sec += 0x10 + ((size + 0xF) & ~size_t(0xF));
    }
    Msbt out;
    for (const auto& [idx, name] : labels) {
        if (idx < strings.size()) out.raw.emplace(name, strings[idx]);
    }
    return out;
}

/// UTF-8 encode one code point (bounded; invalid/surrogate input is replaced with U+FFFD by the
/// caller before this is reached).
void AppendUtf8(std::string& out, char32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(char(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

/// Plain text only: fails closed (nullopt) if the label is absent, empty, contains an MSBT
/// control-tag introducer (0x0E/0x0F -- none of the labels this file reads should carry one; a
/// decoder that silently dropped or mis-rendered a tag would be worse than "unavailable"), a tab
/// (would corrupt the TSV) or an unpaired UTF-16 surrogate. A literal U+000A is re-escaped to the
/// two ASCII characters "\" "n", matching ParseTaskTable's own unescaping convention for hints.
std::optional<std::string> PlainText(const Msbt& m, std::string_view label) {
    const auto it = m.raw.find(std::string{label});
    if (it == m.raw.end() || it->second.empty()) return std::nullopt;
    std::string out;
    const auto& s = it->second;
    for (size_t i = 0; i < s.size(); ++i) {
        const char16_t c = s[i];
        if (c == 0x0E || c == 0x0F || c == '\t') return std::nullopt;
        if (c == '\n') { out += "\\n"; continue; }
        if (c == '\r') continue;
        char32_t cp = c;
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= s.size() || s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF) return std::nullopt;
            cp = 0x10000 + ((char32_t(c) - 0xD800) << 10) + (char32_t(s[i + 1]) - 0xDC00);
            ++i;
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            return std::nullopt;
        }
        AppendUtf8(out, cp);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// small TSV-writing helpers
// ---------------------------------------------------------------------------------------------

std::string Num(double v) {
    // Matches Python's csv writer on a float value read straight out of a BYML scalar (the BYML
    // reader promotes it from float32 bits to double exactly, with no rounding -- see Node::f):
    // Python's str() prints the shortest decimal that round-trips to that exact double, e.g.
    // "10854.0888671875", not a truncated "10854.0889". std::to_chars with no precision argument
    // is the C++17 equivalent (shortest round-trip); verified against the shipped tables row for
    // row during this port's verification (see the test).
    char buf[64];
    const auto result = std::to_chars(buf, buf + sizeof buf, v);
    std::string s(buf, result.ptr);
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos && s.find("nan") == std::string::npos) {
        s += ".0";
    }
    return s;
}
std::string Num(int64_t v) { return std::to_string(v); }

bool Position(const SmoSave::Node* trans, std::array<double, 3>& out) {
    if (!trans) return false;
    const char* keys[] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
        const auto* v = trans->Find(keys[i]);
        if (!v || v->type != SmoSave::Node::Type::Float) return false;
        out[i] = v->f;
    }
    return true;
}

std::string_view Str(const SmoSave::Node& parent, const char* key) {
    const auto* v = parent.Find(key);
    return v && v->type == SmoSave::Node::Type::String ? std::string_view{v->s} : std::string_view{};
}
std::optional<int64_t> IntField(const SmoSave::Node& parent, const char* key) {
    const auto* v = parent.Find(key);
    if (!v || v->type != SmoSave::Node::Type::Int) return std::nullopt;
    return v->i;
}
bool BoolField(const SmoSave::Node& parent, const char* key) {
    const auto* v = parent.Find(key);
    return v && ((v->type == SmoSave::Node::Type::Bool && v->b) ||
                 (v->type == SmoSave::Node::Type::Int && v->i != 0));
}

// ---------------------------------------------------------------------------------------------
// WorldList.szs: world catalogue, home_to_world, stage2world (ShineList primary, WorldResource
// fallback -- exact two-pass join from extract_system_data.py's main()/extract_kingdoms()).
// ---------------------------------------------------------------------------------------------

struct WorldEntry {
    std::string key;         // WorldName, e.g. "Cap"
    std::string home_stage;  // Name, e.g. "CapWorldHomeStage"
    int64_t scenario_num = 0, clear_main = 0, moon_rock = 0, after_ending = 0;
    int64_t coin_capacity = 0;
};

struct WorldCatalog {
    bool ok = false;
    std::vector<WorldEntry> worlds; // WorldList.byml order
    std::unordered_map<std::string, std::string> home_to_world; // home_stage -> world key
    const Archive* world_list_archive = nullptr;
};

WorldCatalog BuildWorldCatalog(const Archive& wl) {
    WorldCatalog out;
    const auto world_list = ParseMember(wl, "WorldList.byml");
    const auto coins = ParseMember(wl, "CollectCoinNum.byml");
    if (!world_list || !world_list->IsArray() || !coins || !coins->IsArray()) return out;
    std::unordered_map<std::string, int64_t> coin_by_world;
    for (const auto& e : coins->arr) {
        const auto name = Str(e, "WorldName");
        const auto n = IntField(e, "CollectCoinNum");
        if (!name.empty() && n) coin_by_world[std::string{name}] = *n;
    }
    for (const auto& e : world_list->arr) {
        WorldEntry w;
        const auto name = Str(e, "WorldName"), home = Str(e, "Name");
        if (name.empty() || home.empty()) return {};
        w.key = std::string{name};
        w.home_stage = std::string{home};
        const auto scen = IntField(e, "ScenarioNum"), cm = IntField(e, "ClearMainScenario"),
                   mr = IntField(e, "MoonRockScenario"), ae = IntField(e, "AfterEndingScenario");
        if (!scen) return {};
        w.scenario_num = *scen;
        w.clear_main = cm.value_or(0);
        w.moon_rock = mr.value_or(0);
        w.after_ending = ae.value_or(0);
        const auto it = coin_by_world.find(w.key);
        w.coin_capacity = it != coin_by_world.end() ? it->second : 0;
        out.home_to_world[w.home_stage] = w.key;
        out.worlds.push_back(std::move(w));
    }
    out.ok = true;
    return out;
}

/// ShineList_<home>.byml for every home stage in `cat`, in WorldList order (matches the archive's
/// own member order closely enough for determinism; see smo_romfs_tables.h). Returns the raw
/// moon rows (one per ShineList entry) in file order, needed by several builders below.
struct MoonRow {
    std::string world, stage, obj_id, dev_object_name, optional_id;
    uint32_t unique_id = 0;
    bool is_grand = false, is_moon_rock = false;
    uint32_t main_scenario_no = 0;
    std::array<double, 3> pos{};
    bool has_pos = false;
};

std::vector<MoonRow> ExtractMoons(const Archive& shine_info, const WorldCatalog& cat) {
    std::vector<MoonRow> out;
    for (const auto& f : shine_info.files) {
        constexpr std::string_view prefix = "ShineList_", suffix = ".byml";
        if (f.name.size() <= prefix.size() + suffix.size() ||
            f.name.compare(0, prefix.size(), prefix) != 0 ||
            f.name.compare(f.name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        const std::string home = f.name.substr(prefix.size(), f.name.size() - prefix.size() - suffix.size());
        const auto wit = cat.home_to_world.find(home);
        const std::string world = wit != cat.home_to_world.end() ? wit->second : std::string{};
        const auto list = ParseMember(shine_info, f.name);
        if (!list) continue;
        const auto* shine_list = list->Find("ShineList");
        if (!shine_list || !shine_list->IsArray()) continue;
        for (const auto& e : shine_list->arr) {
            MoonRow row;
            row.world = world;
            row.stage = std::string{Str(e, "StageName")};
            row.obj_id = std::string{Str(e, "ObjId")};
            row.dev_object_name = std::string{Str(e, "ObjectName")};
            row.optional_id = std::string{Str(e, "OptionalId")};
            const auto uid = IntField(e, "UniqueId"), msn = IntField(e, "MainScenarioNo");
            if (!uid || *uid <= 0) continue;
            row.unique_id = uint32_t(*uid);
            row.is_grand = BoolField(e, "IsGrand");
            row.is_moon_rock = BoolField(e, "IsMoonRock");
            row.main_scenario_no = msn ? uint32_t(*msn) : 0xFFFFFFFFu;
            row.has_pos = Position(e.Find("Trans"), row.pos);
            out.push_back(std::move(row));
        }
    }
    return out;
}

/// stage -> world, two passes: ShineList home-stage ownership (last write wins, matching the
/// Python dict comprehension over `moons` in file order), then WorldResource.byml's
/// "StageData/<X>Design" / "StageData/<X>Map" resources as a fallback that never overwrites the
/// primary mapping -- and only when a (stage, world) candidate is UNIQUE to one kingdom. A stage
/// name every kingdom's resource list loads (verified: "WorldMapStage" and "HomeShipInsideStage"
/// -- the pause-screen map and the Odyssey's interior -- appear in all 17) is shared UI content,
/// not that kingdom's own stage, and must stay unattributed; only a stage whose "Design"/"Map"
/// resource appears in exactly one kingdom's list is real per-kingdom content.
std::unordered_map<std::string, std::string> BuildStageToWorld(
    const std::vector<MoonRow>& moons, const Archive& world_list_archive, const WorldCatalog& cat) {
    std::unordered_map<std::string, std::string> out;
    for (const auto& m : moons) {
        if (!m.world.empty() && !m.stage.empty()) out[m.stage] = m.world; // last wins
    }
    const auto wr = ParseMember(world_list_archive, "WorldResource.byml");
    if (wr && wr->IsArray()) {
        std::map<std::string, std::set<std::string>> candidates; // stage -> worlds that load it
        for (size_t i = 0; i < wr->arr.size() && i < cat.worlds.size(); ++i) {
            const auto& world = cat.worlds[i].key;
            for (const auto& [scen, res] : wr->arr[i].hash) {
                if (!res.IsArray()) continue;
                for (const auto& rr : res.arr) {
                    const auto name = Str(rr, "Name");
                    constexpr std::string_view prefix = "StageData/";
                    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) continue;
                    const auto rest = name.substr(prefix.size());
                    for (std::string_view suffix : {std::string_view{"Design"}, std::string_view{"Map"}}) {
                        if (rest.size() <= suffix.size() ||
                            rest.compare(rest.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
                        candidates[std::string{rest.substr(0, rest.size() - suffix.size())}].insert(world);
                    }
                }
            }
        }
        for (const auto& [stage, worlds] : candidates) {
            if (worlds.size() == 1) out.emplace(stage, *worlds.begin()); // setdefault: never overwrites the primary mapping
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// task-shine-rules.tsv: per-moon achievement weighting/grouping for the 38 Shine_*/MiniGame_*
// task labels. Reverse-engineered from the native AchievementInfo dispatch (main+3235b0) and its
// 20 per-category comparison routines (main+0x4ed7e0.."+0x4dfd50"): each routine tests a fixed
// ObjectName/OptionalId literal against every collected moon and sums IsGet. Two labels are
// special-cased in the native code itself, not a per-category routine: Shine_Gather (main+4d3d20
// -> 4c9da0, the modern-save acquired-unit reconstruction) and Shine_TravelingPeach (main+4ed8e0,
// a per-world "any copy of this one placement acquired" loop). See this file's own analysis notes
// for the disassembly this table was read from; the literals are the game's own internal object/
// category names (reverse-engineered game LOGIC, the same category of thing as this project's
// other hardcoded getter offsets and symbol names), not bulk extracted game text.
struct ShinePredicate { std::string_view literal; int mode; }; // 1: ObjectName contains; 2: ObjectName equals; both OR OptionalId equals
struct ShineStemRules { std::string_view stem; std::vector<ShinePredicate> predicates; };

const std::vector<ShineStemRules>& ShineStemTable() {
    static const std::vector<ShineStemRules> table = {
        {"Shine_Shine2D", {{"_2D", 1}}},
        {"Shine_TreasureBox", {{"宝箱", 2}, {"順序宝箱", 2}}},
        {"Shine_MusicNote", {{"音符オブジェ", 1}}},
        {"Shine_TimerAthretic", {{"タイマー", 1}, {"TimerAthleticShine", 1}}},
        {"Shine_CaptainKinopio", {{"キノピオ隊長", 1}}},
        {"Shine_CollectAnimalAll", {{"コレクトアニマル監視者", 1}}},
        {"Shine_KuriboGirl", {{"ガールクリボー", 1}}},
        {"Shine_Jugem", {{"魚影", 1}}},
        {"Shine_Seed", {{"育つ草", 1}}},
        {"Shine_Rabbit", {{"ウサギ", 1}}},
        {"Shine_DigPoint", {{"ここ掘れポイント", 1}, {"DigPointShine", 1}, {"ヒント写真", 1}}},
        {"Shine_CapHanger", {{"帽子ひっかけポイント", 1}, {"ドクロ看板", 1}}},
        {"Shine_Bird", {{"鳥", 1}}},
        {"Shine_CostumeRoom", {{"CostumeShine", 1}}},
        {"Shine_CapThrottle", {{"帽子スロット台座", 1}}},
        {"MiniGame_RaceMan", {{"駆けっこ開始Npc", 1}}},
        {"MiniGame_FigureWalker", {{"フィギュアウォーキングNpc", 1}}},
        {"Shine_HideAndSeekCapMan", {{"かくれんぼ帽子の民", 1}}},
        {"Shine_HintPhoto", {{"ヒント写真", 1}}},
        {"Shine_CollectBgm", {{"CollectBgmNpcShine", 1}}},
    };
    return table;
}

/// Native main+4d0c40: mode 1 is ObjectName *contains* the literal, mode 2 is ObjectName *equals*
/// it; either mode also accepts OptionalId *equals* the literal. Sums over every predicate in the
/// group (never clamped to 1), matching the native routine exactly.
int ShineWeight(const MoonRow& m, const std::vector<ShinePredicate>& terms) {
    int w = 0;
    for (const auto& term : terms) {
        const bool object_match = term.mode == 1 ? m.dev_object_name.find(term.literal) != std::string::npos
                                                  : m.dev_object_name == term.literal;
        if (object_match || (!m.optional_id.empty() && m.optional_id == term.literal)) ++w;
    }
    return w;
}

/// The Shine_Gather_1/2/3 achievements aren't a fixed-literal category like the 20 above: the
/// native modern-save loader sums a grand-placement bonus (weight 3), skips a shop placement
/// entirely (weight 0 -- ShopShine already counts its first purchase), and weighs every other
/// placement 1.
int GatherWeight(const MoonRow& m) {
    if (m.is_grand) return 3;
    constexpr std::string_view kShopClerk = "\xe3\x82\xb7\xe3\x83\xa7\xe3\x83\x83\xe3\x83\x97\xe5\xba\x97\xe5\x93\xa1";
    return m.dev_object_name.find(kShopClerk) != std::string::npos ? 0 : 1;
}

/// Shine_TravelingPeach_1/2: one specific placement, repeated once per kingdom.
bool IsTravelingPeachMoon(const MoonRow& m) {
    constexpr std::string_view kWorldTravelingPeach =
        "\xe4\xb8\x96\xe7\x95\x8c\xe6\x97\x85\xe8\xa1\x8c\xe3\x83\x94\xe3\x83\xbc\xe3\x83\x81";
    return m.dev_object_name == kWorldTravelingPeach;
}

/// Strips a task label's trailing "_<digits>" tier suffix (e.g. "Shine_Shine2D_1" ->
/// "Shine_Shine2D"), matching the native dispatch: every numbered tier of one achievement family
/// shares the same per-moon weight table.
std::string ShineStem(std::string_view label) {
    size_t end = label.size();
    while (end > 0 && label[end - 1] >= '0' && label[end - 1] <= '9') --end;
    if (end < label.size() && end > 0 && label[end - 1] == '_') return std::string{label.substr(0, end - 1)};
    return std::string{label};
}

/// Appends this label's rows to `rows`, or none if the label isn't one of the 38 Shine_*/
/// MiniGame_* achievements. `resolve_world` maps a moon to its owning kingdom (see Build()'s
/// ResolvedWorld).
template <typename ResolveWorldFn>
void AppendShineRows(const std::string& label, const std::string& stem,
                      const std::vector<ShineStemRules>& stems, const std::vector<MoonRow>& moons,
                      ResolveWorldFn resolve_world,
                      std::vector<std::tuple<std::string, uint32_t, int, std::string>>& rows) {
    const auto group_it = std::find_if(stems.begin(), stems.end(),
        [&](const ShineStemRules& g) { return g.stem == stem; });
    if (group_it != stems.end()) {
        std::vector<std::pair<uint32_t, int>> weighted;
        for (const auto& m : moons) {
            const int w = ShineWeight(m, group_it->predicates);
            if (w > 0) weighted.emplace_back(m.unique_id, w);
        }
        std::sort(weighted.begin(), weighted.end());
        for (const auto& [uid, w] : weighted) rows.emplace_back(label, uid, w, std::string{});
        return;
    }
    if (stem == "Shine_Gather") {
        std::vector<const MoonRow*> order;
        for (const auto& m : moons) order.push_back(&m);
        std::sort(order.begin(), order.end(),
                  [](const MoonRow* a, const MoonRow* b) { return a->unique_id < b->unique_id; });
        for (const auto* m : order) {
            const int w = GatherWeight(*m);
            if (w > 0) rows.emplace_back(label, m->unique_id, w, std::string{});
        }
        return;
    }
    if (stem == "Shine_TravelingPeach") {
        std::vector<std::pair<uint32_t, std::string>> peach;
        for (const auto& m : moons) {
            if (IsTravelingPeachMoon(m)) peach.emplace_back(m.unique_id, resolve_world(m));
        }
        std::sort(peach.begin(), peach.end());
        for (const auto& [uid, world] : peach) rows.emplace_back(label, uid, 1, world);
    }
}

} // namespace

// ===============================================================================================
// Build()
// ===============================================================================================

Tables Build(const EdenDsmodHostApi& host) {
    Tables t;

    const auto world_list = LoadArchive(host, "base:/SystemData/WorldList.szs");
    const auto shine_info = LoadArchive(host, "base:/SystemData/ShineInfo.szs");
    const auto checkpoint_info = LoadArchive(host, "base:/SystemData/CheckpointFlagInfo.szs");
    const auto achievement_info = LoadArchive(host, "base:/SystemData/AchievementInfo.szs");
    const auto hack_obj_list = LoadArchive(host, "base:/SystemData/HackObjList.szs");
    const auto item_list = LoadArchive(host, "base:/SystemData/ItemList.szs");
    const auto system_message = LoadArchive(host, "base:/LocalizedData/USen/MessageData/SystemMessage.szs");
    const auto stage_message = LoadArchive(host, "base:/LocalizedData/USen/MessageData/StageMessage.szs");

    if (!world_list || !shine_info || !checkpoint_info || !achievement_info || !hack_obj_list ||
        !item_list || !system_message || !stage_message) {
        return t; // fail closed: every table stays empty
    }

    const auto cat = BuildWorldCatalog(*world_list);
    if (!cat.ok) return t;
    const auto moons = ExtractMoons(*shine_info, cat);
    const auto stage2world = BuildStageToWorld(moons, *world_list, cat);
    // The ~10 moons whose own home stage owns no WorldList entry (e.g. the "Attack"/Ruined
    // Kingdom boss-rematch moons, filed under ShineList_BossRaidWorldHomeStage.byml, whose "home"
    // is not itself any kingdom's WorldList Name) get their world from the SAME WorldResource
    // fallback stage2world uses (verified against the shipped moons.tsv: unique_id 795 reads
    // "Attack", not "" -- data/README.md's "ten formerly unassigned moon placements belong to
    // Ruined Kingdom (Attack)"). Used for moons.tsv / kingdoms.tsv / world-objectives.tsv, which
    // all want every moon's owning kingdom; world-map-moons.tsv (direct HOME-STAGE overworld map
    // markers only) deliberately keeps each moon's un-fallen-back `world` instead -- verified
    // against the same golden table, which has zero "Attack" rows: these moons are placed on
    // "BossRaidWorldHomeStage", which *is* Attack's nominal `home_stage` field, but Attack has no
    // real overworld map to put a marker on, and the un-fallen-back world is empty for exactly
    // this group, which is what excludes them here without a hardcoded kingdom-name exception.
    const auto ResolvedWorld = [&](const MoonRow& m) -> std::string {
        if (!m.world.empty()) return m.world;
        const auto it = stage2world.find(m.stage);
        return it != stage2world.end() ? it->second : std::string{};
    };

    // ---- SystemMessage: AchievementName/Hint, HackList, StageName -----------------------------
    std::optional<Msbt> achievement_name, achievement_hint, hack_list, stage_name;
    if (auto b = MemberBytes(*system_message, "AchievementName.msbt"); !b.empty()) achievement_name = ParseMsbt(b);
    if (auto b = MemberBytes(*system_message, "AchievementHint.msbt"); !b.empty()) achievement_hint = ParseMsbt(b);
    if (auto b = MemberBytes(*system_message, "HackList.msbt"); !b.empty()) hack_list = ParseMsbt(b);
    if (auto b = MemberBytes(*system_message, "StageName.msbt"); !b.empty()) stage_name = ParseMsbt(b);

    // ======================================================================================
    // kingdoms.tsv: world, world_name, moon_total, coin_capacity, world_id, shine_type
    // ======================================================================================
    {
        std::map<std::string, int> moon_total;
        for (const auto& m : moons) if (const auto w = ResolvedWorld(m); !w.empty()) ++moon_total[w];
        // Mushroom Kingdom's (Peach's) in-game Moon list awards one extra list slot per Toadette
        // achievement reward, on top of its 43 direct placements (data README: "Mushroom
        // Kingdom's list total is 104: 43 placements plus 61 Toadette rewards"). AchievementInfo
        // is already loaded (achievements.tsv is built later in this function); count it here too
        // rather than hardcoding 61, so a future game update that adds/removes an achievement
        // keeps this in sync automatically.
        if (const auto info = ParseMember(*achievement_info, "AchievementInfo.byml")) {
            if (const auto* arr = info->Find("AchievementInfoArray"); arr && arr->IsArray()) {
                moon_total["Peach"] += int(arr->arr.size());
            }
        }
        // shine_type: WorldItemTypeList.byml's own per-kingdom "Shine" field (which map-marker
        // glyph style to use), clamped to the package's validated 0-10 range (ParseKingdomTables)
        // -- Peach's native value is the sentinel 0xFFFFFFFF ("no specific icon"), which reads as
        // the generic/0 glyph, matching the shipped table exactly.
        std::map<std::string, int> shine_type;
        if (const auto wit = ParseMember(*item_list, "WorldItemTypeList.byml"); wit && wit->IsArray()) {
            for (const auto& e : wit->arr) {
                const auto world = Str(e, "WorldName");
                const auto shine = IntField(e, "Shine");
                if (world.empty() || !shine) continue;
                shine_type[std::string{world}] = (*shine >= 0 && *shine <= 10) ? int(*shine) : 0;
            }
        }
        std::string s = "world\tworld_name\tmoon_total\tcoin_capacity\tworld_id\tshine_type\n";
        bool ok = true;
        for (size_t i = 0; i < cat.worlds.size() && ok; ++i) {
            const auto& w = cat.worlds[i];
            std::string name;
            if (stage_name) {
                if (auto n = PlainText(*stage_name, "WorldName_" + w.key)) name = *n; else ok = false;
            } else ok = false;
            const int total = moon_total.contains(w.key) ? moon_total[w.key] : 0;
            const int shine = shine_type.contains(w.key) ? shine_type[w.key] : 0;
            s += w.key + "\t" + name + "\t" + std::to_string(total) + "\t" +
                 std::to_string(w.coin_capacity) + "\t" + std::to_string(i) + "\t" + std::to_string(shine) + "\n";
        }
        if (ok) { t.kingdoms = std::move(s); t.system_data_ok = true; }
    }

    // ======================================================================================
    // moons.tsv: unique_id, world (join key only -- every direct-placement moon, including the
    // ~10 attributed only through the WorldResource fallback -- see ResolvedWorld above)
    // ======================================================================================
    {
        std::string s = "unique_id\tworld\n";
        for (const auto& m : moons) s += std::to_string(m.unique_id) + "\t" + ResolvedWorld(m) + "\n";
        t.moons = std::move(s);
    }

    // ======================================================================================
    // purple-coins.tsv: unique_id, world  (WorldList.szs -> CoinCollectList.byml)
    // ======================================================================================
    {
        const auto coin_list = ParseMember(*world_list, "CoinCollectList.byml");
        const auto* table = coin_list ? coin_list->Find("Table") : nullptr;
        if (table && table->IsArray()) {
            std::string s = "unique_id\tworld\n";
            bool ok = true;
            for (const auto& e : table->arr) {
                const auto uid = IntField(e, "UniqueId");
                const auto stage = Str(e, "StageName");
                if (!uid || *uid <= 0) { ok = false; break; }
                const auto it = stage2world.find(std::string{stage});
                if (it == stage2world.end()) continue; // same as the offline script's blank `world`
                s += std::to_string(*uid) + "\t" + it->second + "\n";
            }
            if (ok) t.purple_coins = std::move(s);
        }
    }

    // ======================================================================================
    // achievements.tsv (label,name) / task-details.tsv (label,name,hint,target)
    // AchievementInfo.szs -> AchievementInfo.byml -> AchievementInfoArray
    // ======================================================================================
    if (achievement_name && achievement_hint) {
        const auto info = ParseMember(*achievement_info, "AchievementInfo.byml");
        const auto* arr = info ? info->Find("AchievementInfoArray") : nullptr;
        if (arr && arr->IsArray()) {
            std::string ach = "label\tname\n", det = "label\tname\thint\ttarget\n";
            bool ok = true;
            for (const auto& e : arr->arr) {
                const auto label = Str(e, "Name");
                if (label.empty()) { ok = false; break; }
                const auto name = PlainText(*achievement_name, label);
                const auto hint = PlainText(*achievement_hint, label);
                if (!name || !hint) { ok = false; break; }
                const auto num = IntField(e, "Num");
                ach += std::string(label) + "\t" + *name + "\n";
                det += std::string(label) + "\t" + *name + "\t" + *hint + "\t" +
                       (num && *num > 0 ? std::to_string(*num) : std::string{}) + "\n";
            }
            if (ok) { t.achievements = std::move(ach); t.task_details = std::move(det); }
        }
    }

    // ======================================================================================
    // captures.tsv: hack_name, capture_name -- HackObjList.szs, IsDebug == 0 only
    // ======================================================================================
    if (hack_list) {
        const auto list = ParseMember(*hack_obj_list, "HackObjList.byml");
        if (list && list->IsArray()) {
            std::string s = "hack_name\tcapture_name\n";
            bool ok = true;
            for (const auto& e : list->arr) {
                if (BoolField(e, "IsDebug")) continue;
                const auto hn = Str(e, "HackName");
                if (hn.empty()) { ok = false; break; }
                const auto name = PlainText(*hack_list, hn);
                if (!name) continue; // not every debug-excluded entry has a display name (none observed, fail-open-per-row)
                s += std::string(hn) + "\t" + *name + "\n";
            }
            if (ok) t.captures = std::move(s);
        }
    }

    // ======================================================================================
    // costumes-cap.tsv / costumes-cloth.tsv / items-gift.tsv
    // character_id/numbering_id/amiibo are always "" -- see smo_romfs_tables.h.
    // ======================================================================================
    {
        const auto build_items5 = [&](const char* member) -> std::optional<std::string> {
            const auto list = ParseMember(*item_list, member);
            if (!list || !list->IsArray()) return std::nullopt;
            std::string s = "item_name\tdisplay_name\tcharacter_id\tnumbering_id\tamiibo\n";
            for (const auto& e : list->arr) {
                const auto name = Str(e, "ItemName"), disp = Str(e, "DisplayName");
                if (name.empty()) return std::nullopt;
                s += std::string(name) + "\t" + std::string(disp) + "\t\t\t\n";
            }
            return s;
        };
        if (auto s = build_items5("ItemCap.byml")) t.costumes_cap = std::move(*s);
        if (auto s = build_items5("ItemCloth.byml")) t.costumes_cloth = std::move(*s);
        const auto gift = ParseMember(*item_list, "ItemGift.byml");
        if (gift && gift->IsArray()) {
            std::string s = "item_name\tdisplay_name\n";
            bool ok = true;
            for (const auto& e : gift->arr) {
                const auto name = Str(e, "ItemName");
                if (name.empty()) { ok = false; break; }
                s += std::string(name) + "\t" + std::string(Str(e, "DisplayName")) + "\n";
            }
            if (ok) t.items_gift = std::move(s);
        }
    }

    // ======================================================================================
    // task-item-rules.tsv (save_array,item_name): ItemGift.byml + ItemSticker.byml
    // task-shop-rules.tsv (world,save_array,item_name): ItemList.byml regional-coin rows
    // task-world-rules.tsv: world_id,world,clear_main,after_ending,moon_rock,sphinx_field
    // ======================================================================================
    {
        std::string s = "save_array\titem_name\n";
        bool ok = true;
        const auto add_array = [&](const char* member, const char* array_name) {
            const auto list = ParseMember(*item_list, member);
            if (!list || !list->IsArray()) { ok = false; return; }
            for (const auto& e : list->arr) {
                const auto name = Str(e, "ItemName");
                if (name.empty()) { ok = false; return; }
                s += std::string(array_name) + "\t" + std::string(name) + "\n";
            }
        };
        add_array("ItemGift.byml", "ItemGift");
        add_array("ItemSticker.byml", "ItemSticker");
        if (ok) t.task_item_rules = std::move(s);
    }
    {
        const auto list = ParseMember(*item_list, "ItemList.byml");
        if (list && list->IsArray()) {
            std::string s = "world\tsave_array\titem_name\n";
            bool ok = true;
            static const std::unordered_map<std::string_view, std::string_view> kArrayByType = {
                {"Cap", "ItemCap"}, {"Clothes", "ItemCloth"}, {"Sticker", "ItemSticker"},
                {"Gift", "ItemGift"}};
            for (const auto& e : list->arr) {
                const auto coin_type = Str(e, "CoinType");
                if (coin_type != "Collect") continue; // regional (purple) coin purchases only
                const auto world = Str(e, "StoreName"); // the shop's own kingdom, not ClearWorld
                const auto item_type = Str(e, "ItemType");
                const auto name = Str(e, "ItemName");
                if (world.empty() || name.empty()) continue;
                const auto it = kArrayByType.find(item_type);
                if (it == kArrayByType.end()) continue;
                s += std::string(world) + "\t" + std::string(it->second) + "\t" + std::string(name) + "\n";
            }
            if (ok) t.task_shop_rules = std::move(s);
        }
    }
    {
        // sphinx_field: an investigator-assigned reference to the save's own SphinxQuizData[]
        // field name (see ApplyTaskRules in smo_save.cpp), not game-extracted text. Forest's quiz
        // is single-flag; every other kingdom's is the "answered every quiz" aggregate flag (data
        // README: "Sphinx uses Forest's first quiz flag and the All flag elsewhere").
        std::string s = "world_id\tworld\tclear_main\tafter_ending\tmoon_rock\tsphinx_field\n";
        for (size_t i = 0; i < cat.worlds.size(); ++i) {
            const auto& w = cat.worlds[i];
            const char* field = w.key == "Forest" ? "IsAnswerCorrectSphinxQuiz" : "IsAnswerCorrectSphinxQuizAll";
            s += std::to_string(i) + "\t" + w.key + "\t" + std::to_string(w.clear_main) + "\t" +
                 std::to_string(w.after_ending) + "\t" + std::to_string(w.moon_rock) + "\t" + field + "\n";
        }
        t.task_world_rules = std::move(s);
    }
    // task-shine-rules.tsv: label, unique_id, weight, group. See ShineStemTable() above.
    {
        const auto info = ParseMember(*achievement_info, "AchievementInfo.byml");
        const auto* arr = info ? info->Find("AchievementInfoArray") : nullptr;
        bool shine_ok = arr && arr->IsArray();
        std::vector<std::tuple<std::string, uint32_t, int, std::string>> rows;
        if (shine_ok) {
            const auto& stems = ShineStemTable();
            for (const auto& e : arr->arr) {
                const auto label = Str(e, "Name");
                if (label.empty()) { shine_ok = false; break; }
                AppendShineRows(std::string(label), ShineStem(label), stems, moons, ResolvedWorld, rows);
            }
        }
        if (shine_ok) {
            std::string s = "label\tunique_id\tweight\tgroup\n";
            for (const auto& [label, uid, w, group] : rows)
                s += label + "\t" + std::to_string(uid) + "\t" + std::to_string(w) + "\t" + group + "\n";
            t.task_shine_rules = std::move(s);
        }
    }

    // ======================================================================================
    // world-maps.tsv: world_id, world, home_stage, name, region, scenario_count, after_ending, moon_rock
    // ======================================================================================
    if (stage_name) {
        std::string s = "world_id\tworld\thome_stage\tname\tregion\tscenario_count\tafter_ending\tmoon_rock\n";
        bool ok = true;
        for (size_t i = 0; i < cat.worlds.size() && ok; ++i) {
            const auto& w = cat.worlds[i];
            const auto name = PlainText(*stage_name, "WorldName_" + w.key);
            const auto region = PlainText(*stage_name, "RegionName_" + w.key);
            if (!name || !region) { ok = false; break; }
            s += std::to_string(i) + "\t" + w.key + "\t" + w.home_stage + "\t" + *name + "\t" + *region + "\t" +
                 std::to_string(w.scenario_num) + "\t" + std::to_string(w.after_ending) + "\t" +
                 std::to_string(w.moon_rock) + "\n";
        }
        if (ok) t.world_maps = std::move(s);
    }

    // ======================================================================================
    // world-stages.tsv: stage, world, category ("" -- see smo_romfs_tables.h)
    // ======================================================================================
    {
        std::string s = "stage\tworld\tcategory\n";
        std::set<std::string> seen;
        for (const auto& [stage, world] : stage2world) {
            if (!seen.insert(stage).second) continue;
            s += stage + "\t" + world + "\t\n";
        }
        t.world_stages = std::move(s);
    }

    // ======================================================================================
    // world-map-invalid.tsv: stage, scenario -- WorldList.szs -> InvalidOpenMapList.byml
    // ======================================================================================
    {
        const auto list = ParseMember(*world_list, "InvalidOpenMapList.byml");
        if (list && list->IsArray()) {
            std::string s = "stage\tscenario\n";
            bool ok = true;
            for (const auto& e : list->arr) {
                const auto stage = Str(e, "Name");
                const auto scen = IntField(e, "Scenario");
                if (stage.empty() || !scen) { ok = false; break; }
                s += std::string(stage) + "\t" + std::to_string(*scen) + "\n";
            }
            if (ok) t.world_map_invalid = std::move(s);
        }
    }

    // ======================================================================================
    // world-map-anchors.tsv: stage, scenario, x, y, z -- WorldList.szs -> StagePosList.byml
    // ======================================================================================
    {
        const auto list = ParseMember(*world_list, "StagePosList.byml");
        if (list && list->IsHash()) {
            std::string s = "stage\tscenario\tx\ty\tz\n";
            for (const auto& [stage, scenarios] : list->hash) {
                if (!scenarios.IsHash()) continue;
                for (const auto& [scen, pos] : scenarios.hash) {
                    std::array<double, 3> xyz{};
                    if (!pos.IsHash() || !Position(&pos, xyz)) continue;
                    s += stage + "\t" + scen + "\t" + Num(xyz[0]) + "\t" + Num(xyz[1]) + "\t" + Num(xyz[2]) + "\n";
                }
            }
            t.world_map_anchors = std::move(s);
        }
    }

    // ======================================================================================
    // world-map-moons.tsv: world, stage, unique_id, is_moon_rock, main_scenario_no, x, y, z
    // Direct home-stage moons only (smo_world.h's Moon contract).
    // ======================================================================================
    {
        std::string s = "world\tstage\tunique_id\tis_moon_rock\tmain_scenario_no\tx\ty\tz\n";
        std::unordered_map<std::string, std::string> home_by_world;
        for (const auto& w : cat.worlds) home_by_world[w.key] = w.home_stage;
        for (const auto& m : moons) {
            if (m.world.empty() || !m.has_pos) continue;
            const auto it = home_by_world.find(m.world);
            if (it == home_by_world.end() || m.stage != it->second) continue;
            s += m.world + "\t" + m.stage + "\t" + std::to_string(m.unique_id) + "\t" +
                 (m.is_moon_rock ? "1" : "0") + "\t" + std::to_string(m.main_scenario_no) + "\t" +
                 Num(m.pos[0]) + "\t" + Num(m.pos[1]) + "\t" + Num(m.pos[2]) + "\n";
        }
        t.world_map_moons = std::move(s);
    }

    // ======================================================================================
    // world-objectives.tsv: world, main_shine_no, unique_id, stage, id, label, name, x, y, z
    // Story moons only (main_scenario_no != 0xFFFFFFFF); any stage, not home-stage-only (unlike
    // world-map-moons.tsv) -- a story moon can be placed in a sub-stage.
    // ======================================================================================
    {
        std::string s = "world\tmain_shine_no\tunique_id\tstage\tid\tlabel\tname\tx\ty\tz\n";
        bool ok = true;
        for (const auto& m : moons) {
            const auto world = ResolvedWorld(m);
            if (world.empty() || m.main_scenario_no == 0xFFFFFFFFu || !m.has_pos) continue;
            const std::string label = "ScenarioName_" + m.obj_id;
            if (!stage_name) { ok = false; break; } // shares SystemMessage's presence, not its content
            // Objective names live in the per-stage StageMessage member, not SystemMessage.
            const auto member = MemberBytes(*stage_message, m.stage + ".msbt");
            if (member.empty()) continue;
            const auto parsed = ParseMsbt(member);
            if (!parsed) continue;
            const auto name = PlainText(*parsed, label);
            if (!name) continue;
            s += world + "\t" + std::to_string(m.main_scenario_no) + "\t" + std::to_string(m.unique_id) + "\t" +
                 m.stage + "\t" + m.obj_id + "\t" + label + "\t" + *name + "\t" +
                 Num(m.pos[0]) + "\t" + Num(m.pos[1]) + "\t" + Num(m.pos[2]) + "\n";
        }
        if (ok) t.world_objectives = std::move(s);
    }

    // ======================================================================================
    // checkpoints.tsv (legacy, Sand only; dead field downstream -- see smo_romfs_tables.h) /
    // world-checkpoints.tsv (world,stage,scenario,id,x,y,z,name) /
    // world-checkpoint-scenarios.tsv (world,stage,scenario,count, including empty scenarios) --
    // all from CheckpointFlagInfo.szs -> FlagList_<stage>_<scenario>.byml, names from the per-
    // stage StageMessage member's Checkpoint_obj<N> labels.
    // ======================================================================================
    {
        struct FlagRow { std::string stage, scen, id; std::array<double, 3> pos{}; };
        std::vector<FlagRow> flags;
        std::vector<std::pair<std::string, std::string>> scenarios; // (stage, scen), every member incl. empty
        bool ok = true;
        for (const auto& f : checkpoint_info->files) {
            constexpr std::string_view prefix = "FlagList_", suffix = ".byml";
            if (f.name.size() <= prefix.size() + suffix.size() ||
                f.name.compare(0, prefix.size(), prefix) != 0 ||
                f.name.compare(f.name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            const std::string stem = f.name.substr(prefix.size(), f.name.size() - prefix.size() - suffix.size());
            const auto us = stem.rfind('_');
            if (us == std::string::npos) { ok = false; break; }
            const std::string stage = stem.substr(0, us), scen = stem.substr(us + 1);
            scenarios.emplace_back(stage, scen);
            const auto parsed = ParseMember(*checkpoint_info, f.name);
            if (!parsed) continue;
            const auto* list = parsed->Find("FlagList");
            if (!list || !list->IsArray()) continue; // an explicit empty/absent list: zero checkpoints here
            for (const auto& e : list->arr) {
                const auto id = Str(e, "FlagIdStr");
                std::array<double, 3> xyz{};
                if (id.empty() || !Position(e.Find("Trans"), xyz)) continue;
                flags.push_back({stage, scen, std::string{id}, xyz});
            }
        }
        if (ok) {
            // world-checkpoint-scenarios.tsv
            std::map<std::pair<std::string, std::string>, int> count_by_scenario;
            for (const auto& [stage, scen] : scenarios) count_by_scenario[{stage, scen}] = 0;
            for (const auto& f : flags) ++count_by_scenario[{f.stage, f.scen}];
            std::string scen_tsv = "world\tstage\tscenario\tcount\n";
            bool scen_ok = true;
            for (const auto& [key, count] : count_by_scenario) {
                const auto wit = stage2world.find(key.first);
                if (wit == stage2world.end()) { scen_ok = false; break; }
                scen_tsv += wit->second + "\t" + key.first + "\t" + key.second + "\t" + std::to_string(count) + "\n";
            }
            if (scen_ok) t.world_checkpoint_scenarios = std::move(scen_tsv);

            // world-checkpoints.tsv (one row per stage/scenario/flag, with names) and the
            // legacy Sand-only checkpoints.tsv subset.
            std::string wc = "world\tstage\tscenario\tid\tx\ty\tz\tname\n";
            std::string legacy = "stage\tid\tx\ty\tz\tname\n";
            std::set<std::pair<std::string, std::string>> legacy_seen; // (stage, id), dedup like ParseCheckpointTable
            bool wc_ok = true;
            std::unordered_map<std::string, std::optional<Msbt>> names_by_stage;
            for (const auto& f : flags) {
                const auto wit = stage2world.find(f.stage);
                if (wit == stage2world.end()) { wc_ok = false; break; }
                // A flag's own FlagIdStr is sometimes a compound "obj<N>(<ZoneStage>[obj<M>])"
                // (the game's own data, not a transformation): the flag is placed in a "Zone"
                // sub-stage linked into this home stage, and its Checkpoint_obj<N> label lives in
                // the ZONE's own StageMessage member, not the home stage's. The id column keeps
                // the full compound string; only the name lookup's (stage, label) pair changes.
                std::string label_stage = f.stage, label_id = f.id;
                if (const auto paren = f.id.find('('); paren != std::string::npos) {
                    const auto bracket = f.id.find('[', paren);
                    if (bracket == std::string::npos) { wc_ok = false; break; }
                    label_stage = f.id.substr(paren + 1, bracket - paren - 1);
                    label_id = f.id.substr(0, paren);
                }
                if (names_by_stage.find(label_stage) == names_by_stage.end()) {
                    const auto member = MemberBytes(*stage_message, label_stage + ".msbt");
                    names_by_stage[label_stage] = member.empty() ? std::nullopt : ParseMsbt(member);
                }
                const auto& msbt = names_by_stage.at(label_stage);
                if (!msbt) { wc_ok = false; break; }
                const auto name = PlainText(*msbt, "Checkpoint_" + label_id);
                if (!name) continue; // a flag with no native name (none expected; skip, not fail)
                wc += wit->second + "\t" + f.stage + "\t" + f.scen + "\t" + f.id + "\t" +
                      Num(f.pos[0]) + "\t" + Num(f.pos[1]) + "\t" + Num(f.pos[2]) + "\t" + *name + "\n";
                if (wit->second == "Sand" && legacy_seen.emplace(f.stage, f.id).second) {
                    legacy += f.stage + "\t" + f.id + "\t" + Num(f.pos[0]) + "\t" + Num(f.pos[1]) + "\t" +
                              Num(f.pos[2]) + "\t" + *name + "\n";
                }
            }
            if (wc_ok) { t.world_checkpoints = std::move(wc); t.checkpoints = std::move(legacy); }
        }
    }

    // ======================================================================================
    // world-map-icons.tsv: world, stage, scenario, id, kind, store, minigame, x, y, z --
    // StageData/<HomeStage>Map.szs -> <HomeStage>Map.byml, root[i].MapIconList (scenario = i+1,
    // capped at the kingdom's own scenario_num: some stages' root array carries a few trailing
    // copies of the last real scenario past that point). Uses TargetedByml, not
    // SmoSave::ParseByml / ParseMember: a big kingdom's full 15-scenario object-placement tree is
    // millions of BYML nodes (City, measured: ~8.3M) -- far past ParseByml's save-file-sized
    // budget -- while the handful of MapIconList entries this table actually needs are tiny.
    // ======================================================================================
    {
        std::string s = "world\tstage\tscenario\tid\tkind\tstore\tminigame\tx\ty\tz\n";
        bool any_stage_loaded = false, ok = true;
        for (const auto& w : cat.worlds) {
            const auto archive = LoadArchive(host, "base:/StageData/" + w.home_stage + "Map.szs");
            if (!archive) continue; // some kingdoms (Peach, Moon, ...) may have no map icons at all
            const auto member = MemberBytes(*archive, w.home_stage + "Map.byml");
            if (member.empty()) continue;
            TargetedByml byml;
            if (!byml.Load(member)) { ok = false; continue; }
            const auto count = byml.RootArrayCount();
            if (!count) continue;
            any_stage_loaded = true;
            const uint32_t last = std::min<uint32_t>(*count, w.scenario_num);
            for (uint32_t i = 0; i < last; ++i) {
                const auto icons = byml.FindInRootElement(i, "MapIconList");
                if (!icons || !icons->IsArray()) continue;
                for (const auto& e : icons->arr) {
                    const auto id = Str(e, "Id"), kind = Str(e, "UnitConfigName"),
                               store = Str(e, "StoreName"), minigame = Str(e, "MiniGameName");
                    std::array<double, 3> xyz{};
                    if (id.empty() || kind.empty() || !Position(e.Find("Translate"), xyz)) { ok = false; continue; }
                    s += w.key + "\t" + w.home_stage + "\t" + std::to_string(i + 1) + "\t" + std::string(id) + "\t" +
                         std::string(kind) + "\t" + std::string(store) + "\t" + std::string(minigame) + "\t" +
                         Num(xyz[0]) + "\t" + Num(xyz[1]) + "\t" + Num(xyz[2]) + "\n";
                }
            }
        }
        if (any_stage_loaded && ok) { t.world_map_icons = std::move(s); t.map_icons_ok = true; }
    }

    // ======================================================================================
    // world-map-variants.tsv: texture, width, height, projection_member --
    // ObjectData/Texture2dMap.szs's own .byml members, dimensions via SmoMap::DecodeArchive
    // (reused as-is: the same code that decodes the live minimap projection/texture).
    // ======================================================================================
    {
        const auto raw = ReadWholeRomfs(host, "base:/ObjectData/Texture2dMap.szs");
        if (raw) {
            std::string s = "texture\twidth\theight\tprojection_member\n";
            bool any = false, ok = true;
            std::vector<u8> expanded;
            Span archive_span(*raw);
            if (raw->size() >= 4 && std::memcmp(raw->data(), "Yaz0", 4) == 0) {
                auto e = ExpandYaz0(*raw);
                if (e) { expanded = std::move(*e); archive_span = expanded; }
            }
            const auto files = fe3h_ui2d::Sarc(archive_span);
            for (const auto& f : files) {
                constexpr std::string_view suffix = ".byml";
                if (f.name.size() <= suffix.size() ||
                    f.name.compare(f.name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
                const std::string texture = f.name.substr(0, f.name.size() - suffix.size());
                const auto decoded = SmoMap::DecodeArchive(archive_span, texture);
                if (!decoded) { ok = false; continue; }
                any = true;
                s += texture + "\t" + std::to_string(decoded->image.width) + "\t" +
                     std::to_string(decoded->image.height) + "\t" + f.name + "\n";
            }
            if (any && ok) { t.world_map_variants = std::move(s); t.map_variants_ok = true; }
        }
    }

    t.strings_ok = bool(achievement_name) && bool(achievement_hint) && bool(hack_list) && bool(stage_name);
    return t;
}

} // namespace SmoRomfsTables
