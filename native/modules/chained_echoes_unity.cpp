// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Unity asset reader for the Chained Echoes 1.41 companion. Format notes and the runtime design:
// chained_echoes_unity.h. The bundle / SerializedFile / typetree / texture / sprite core is the
// Luminescent Platinum reader's (lp_unity.cpp, lumi-platinum-companion branch) verbatim apart from
// the namespace, so a later shared Unity core can replace both; the Chained Echoes parts (asset
// root, sprite families, TextMesh Pro font, pixel cache, stats) are at the end of this file.

#include "chained_echoes_unity.h"

#include "ce_parse.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>

#include "bc_decoder.h"
#include "lz4.h"

namespace ce_unity {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

constexpr i32 ClassTexture2D = 28;
constexpr i32 ClassFont = 128;
constexpr i32 ClassSprite = 213;
constexpr i32 ClassSpriteAtlas = 687078895;
constexpr i32 PlatformSwitch = 38;
constexpr u32 MaxDim = 16384;
constexpr int MaxDepth = 48;

struct AtomicStats {
    std::atomic<std::uint64_t> romfs_reads{}, romfs_bytes{}, blocks_decompressed{},
        bytes_decompressed{}, bundles_opened{}, objects_indexed{};
};
AtomicStats g_stats;
// Decompressed LZ4 blocks kept per open bundle (lp_unity used 24 MiB; 2 MiB = 16 blocks is enough
// for sequential name indexing and any single texture of this game, and bounds RSS).
std::atomic<std::size_t> g_block_budget{2u * 1024u * 1024u};

void Count(std::atomic<std::uint64_t>& a, std::uint64_t n = 1) {
    a.fetch_add(n, std::memory_order_relaxed);
}

// ---- byte cursor ---------------------------------------------------------------------------

struct Cursor {
    const u8* data{};
    std::size_t size{};
    std::size_t pos{};
    std::size_t base{}; // absolute offset of data[0] (alignment is absolute)
    bool big{};
    bool ok{true};

    bool Need(std::size_t n) {
        if (!ok || n > size - pos) {
            ok = false;
            return false;
        }
        return true;
    }
    std::size_t Remaining() const {
        return size - pos;
    }
    template <typename T>
    T Int() {
        if (!Need(sizeof(T)))
            return T{};
        u64 v = 0;
        for (std::size_t k = 0; k < sizeof(T); ++k) {
            const u64 b = data[pos + k];
            v |= big ? b << (8 * (sizeof(T) - 1 - k)) : b << (8 * k);
        }
        pos += sizeof(T);
        return static_cast<T>(v);
    }
    float F32() {
        const u32 v = Int<u32>();
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    double F64() {
        const u64 v = Int<u64>();
        double f;
        std::memcpy(&f, &v, 8);
        return f;
    }
    void Align(std::size_t a = 4) {
        const std::size_t abs = base + pos;
        const std::size_t pad = (a - abs % a) % a;
        if (pad > size - pos) {
            pos = size; // UnityPy tolerates an alignment past the end of the last field
            return;
        }
        pos += pad;
    }
    std::string CStr() {
        const void* end = ok && pos < size ? std::memchr(data + pos, 0, size - pos) : nullptr;
        if (!end) {
            ok = false;
            return {};
        }
        const std::size_t n = static_cast<const u8*>(end) - (data + pos);
        std::string s(reinterpret_cast<const char*>(data + pos), n);
        pos += n + 1;
        return s;
    }
    bool Bytes(std::string& out, std::size_t n) {
        if (!Need(n))
            return false;
        out.assign(reinterpret_cast<const char*>(data + pos), n);
        pos += n;
        return true;
    }
    void Skip(std::size_t n) {
        if (Need(n))
            pos += n;
    }
};

double PyRound(double v) {
    return std::nearbyint(v); // default rounding mode: half to even, like Python's round()
}

/// static_cast<T>(v) for a value read from a file: NaN gives 0 and values beyond T's range clamp
/// to it (a float -> integer conversion of those is undefined behaviour).
template <class T>
T SaturatingCast(double v) {
    if (std::isnan(v))
        return T{};
    if (v <= static_cast<double>(std::numeric_limits<T>::lowest()))
        return std::numeric_limits<T>::lowest();
    if (v >= static_cast<double>(std::numeric_limits<T>::max()))
        return std::numeric_limits<T>::max();
    return static_cast<T>(v);
}

// ---- Unity's common typetree strings (offset | 0x80000000) ------------------------------------

constexpr char CommonStrings[] =
    "AABB\0AnimationClip\0AnimationCurve\0AnimationState\0Array\0Base\0BitField\0bitset\0bool\0"
    "char\0ColorRGBA\0Component\0data\0deque\0double\0dynamic_array\0FastPropertyName\0first\0"
    "float\0Font\0GameObject\0Generic Mono\0GradientNEW\0GUID\0GUIStyle\0int\0list\0long long\0"
    "map\0Matrix4x4f\0MdFour\0MonoBehaviour\0MonoScript\0m_ByteSize\0m_Curve\0"
    "m_EditorClassIdentifier\0m_EditorHideFlags\0m_Enabled\0m_ExtensionPtr\0m_GameObject\0"
    "m_Index\0m_IsArray\0m_IsStatic\0m_MetaFlag\0m_Name\0m_ObjectHideFlags\0m_PrefabInternal\0"
    "m_PrefabParentObject\0m_Script\0m_StaticEditorFlags\0m_Type\0m_Version\0Object\0pair\0"
    "PPtr<Component>\0PPtr<GameObject>\0PPtr<Material>\0PPtr<MonoBehaviour>\0PPtr<MonoScript>\0"
    "PPtr<Object>\0PPtr<Prefab>\0PPtr<Sprite>\0PPtr<TextAsset>\0PPtr<Texture>\0PPtr<Texture2D>\0"
    "PPtr<Transform>\0Prefab\0Quaternionf\0Rectf\0RectInt\0RectOffset\0second\0set\0short\0size\0"
    "SInt16\0SInt32\0SInt64\0SInt8\0staticvector\0string\0TextAsset\0TextMesh\0Texture\0Texture2D\0"
    "Transform\0TypelessData\0UInt16\0UInt32\0UInt64\0UInt8\0unsigned int\0unsigned long long\0"
    "unsigned short\0vector\0Vector2f\0Vector3f\0Vector4f\0m_ScriptingClassIdentifier\0Gradient\0"
    "Type*\0int2_storage\0int3_storage\0BoundsInt\0m_CorrespondingSourceObject\0m_PrefabInstance\0"
    "m_PrefabAsset\0FileSize\0Hash128\0RenderingLayerMask\0fixed_bitset\0mutable_string\0"
    "m_Owner\0ReferencedObject\0";

std::string CommonString(u32 offset) {
    if (offset >= sizeof(CommonStrings) - 1)
        return std::to_string(offset);
    return std::string{CommonStrings + offset};
}

// ---- typetree ----------------------------------------------------------------------------------

enum class Prim : u8 { None, S8, U8, S16, U16, S32, U32, S64, U64, F32, F64, Bool, String, Typeless };

Prim PrimOf(std::string_view t) {
    static const std::map<std::string_view, Prim> table{
        {"SInt8", Prim::S8},     {"UInt8", Prim::U8},
        {"char", Prim::U8},      {"short", Prim::S16},
        {"SInt16", Prim::S16},   {"unsigned short", Prim::U16},
        {"UInt16", Prim::U16},   {"int", Prim::S32},
        {"SInt32", Prim::S32},   {"unsigned int", Prim::U32},
        {"UInt32", Prim::U32},   {"Type*", Prim::U32},
        {"long long", Prim::S64}, {"SInt64", Prim::S64},
        {"unsigned long long", Prim::U64}, {"UInt64", Prim::U64},
        {"FileSize", Prim::U64}, {"float", Prim::F32},
        {"double", Prim::F64},   {"bool", Prim::Bool},
        {"string", Prim::String}, {"TypelessData", Prim::Typeless},
    };
    const auto it = table.find(t);
    return it == table.end() ? Prim::None : it->second;
}

struct TreeNode {
    std::string type, name;
    int level{};
    u32 meta{};
    Prim prim{};
    std::vector<int> children;
};

struct Tree {
    std::vector<TreeNode> nodes; // nodes[0] = root
};

bool LinkTree(Tree& t) {
    if (t.nodes.empty() || t.nodes[0].level != 0)
        return false;
    std::vector<int> stack{0};
    for (std::size_t i = 1; i < t.nodes.size(); ++i) {
        const int lv = t.nodes[i].level;
        if (lv <= 0 || lv > static_cast<int>(stack.size()))
            return false;
        stack.resize(static_cast<std::size_t>(lv));
        t.nodes[static_cast<std::size_t>(stack.back())].children.push_back(static_cast<int>(i));
        stack.push_back(static_cast<int>(i));
    }
    for (auto& n : t.nodes)
        n.prim = PrimOf(n.type);
    return true;
}

std::optional<Tree> ParseTreeBlob(Cursor& c, u32 version) {
    const i32 count = c.Int<i32>();
    const i32 strsize = c.Int<i32>();
    const std::size_t node_size = version >= 19 ? 32 : 24;
    if (!c.ok || count <= 0 || strsize < 0 || static_cast<u64>(count) * node_size > c.Remaining() ||
        static_cast<u64>(strsize) > c.Remaining() - static_cast<u64>(count) * node_size)
        return std::nullopt;
    Cursor nodes = c;
    c.Skip(static_cast<std::size_t>(count) * node_size);
    const char* strings = reinterpret_cast<const char*>(c.data + c.pos);
    c.Skip(static_cast<std::size_t>(strsize));
    const auto str = [&](u32 v) -> std::string {
        if (v & 0x80000000u)
            return CommonString(v & 0x7FFFFFFFu);
        if (v >= static_cast<u32>(strsize))
            return {};
        const void* end = std::memchr(strings + v, 0, static_cast<std::size_t>(strsize) - v);
        const std::size_t n = end ? static_cast<const char*>(end) - (strings + v)
                                  : static_cast<std::size_t>(strsize) - v;
        return std::string{strings + v, n};
    };
    Tree t;
    t.nodes.resize(static_cast<std::size_t>(count));
    for (auto& n : t.nodes) {
        nodes.Int<u16>(); // version
        n.level = nodes.Int<u8>();
        nodes.Int<u8>(); // type flags
        n.type = str(nodes.Int<u32>());
        n.name = str(nodes.Int<u32>());
        nodes.Int<i32>(); // byte size
        nodes.Int<i32>(); // index
        n.meta = nodes.Int<u32>();
        if (version >= 19)
            nodes.Int<u64>(); // ref type hash
    }
    if (!c.ok || !nodes.ok || !LinkTree(t))
        return std::nullopt;
    return t;
}

/// 2019.4 Font layout (UnityPy TPK, class 128), used when the file carries no typetree.
const Tree& BuiltinFontTree() {
    static const Tree tree = [] {
        struct Row {
            int level;
            const char* type;
            const char* name;
            u32 meta;
        };
        static constexpr Row rows[] = {
            {0, "Font", "Base", 0x8000},
            {1, "string", "m_Name", 0x8000},
            {2, "Array", "Array", 0x4000},
            {3, "int", "size", 0},
            {3, "char", "data", 0},
            {1, "float", "m_LineSpacing", 0},
            {1, "PPtr<Material>", "m_DefaultMaterial", 0},
            {2, "int", "m_FileID", 0},
            {2, "SInt64", "m_PathID", 0},
            {1, "float", "m_FontSize", 0},
            {1, "PPtr<Texture>", "m_Texture", 0x4000},
            {2, "int", "m_FileID", 0},
            {2, "SInt64", "m_PathID", 0},
            {1, "int", "m_AsciiStartOffset", 0},
            {1, "float", "m_Tracking", 0},
            {1, "int", "m_CharacterSpacing", 0},
            {1, "int", "m_CharacterPadding", 0},
            {1, "int", "m_ConvertCase", 0},
            {1, "vector", "m_CharacterRects", 0x8000},
            {2, "Array", "Array", 0xC000},
            {3, "int", "size", 0},
            {3, "CharacterInfo", "data", 0x8000},
            {4, "unsigned int", "index", 0},
            {4, "Rectf", "uv", 0},
            {5, "float", "x", 0},
            {5, "float", "y", 0},
            {5, "float", "width", 0},
            {5, "float", "height", 0},
            {4, "Rectf", "vert", 0},
            {5, "float", "x", 0},
            {5, "float", "y", 0},
            {5, "float", "width", 0},
            {5, "float", "height", 0},
            {4, "float", "advance", 0},
            {4, "bool", "flipped", 0x4000},
            {1, "map", "m_KerningValues", 0},
            {2, "Array", "Array", 0},
            {3, "int", "size", 0},
            {3, "pair", "data", 0},
            {4, "pair", "first", 0},
            {5, "UInt16", "first", 0},
            {5, "UInt16", "second", 0},
            {4, "float", "second", 0},
            {1, "float", "m_PixelScale", 0x4000},
            {1, "vector", "m_FontData", 0xC000},
            {2, "Array", "Array", 0x4000},
            {3, "int", "size", 0},
            {3, "char", "data", 0},
            {1, "float", "m_Ascent", 0},
            {1, "float", "m_Descent", 0},
            {1, "unsigned int", "m_DefaultStyle", 0},
            {1, "vector", "m_FontNames", 0x8000},
            {2, "Array", "Array", 0xC000},
            {3, "int", "size", 0},
            {3, "string", "data", 0x8000},
            {4, "Array", "Array", 0x4000},
            {5, "int", "size", 0},
            {5, "char", "data", 0},
            {1, "vector", "m_FallbackFonts", 0xC000},
            {2, "Array", "Array", 0x4000},
            {3, "int", "size", 0},
            {3, "PPtr<Font>", "data", 0},
            {4, "int", "m_FileID", 0},
            {4, "SInt64", "m_PathID", 0},
            {1, "int", "m_FontRenderingMode", 0},
            {1, "bool", "m_UseLegacyBoundsCalculation", 0},
            {1, "bool", "m_ShouldRoundAdvanceValue", 0},
        };
        Tree t;
        for (const Row& r : rows)
            t.nodes.push_back(TreeNode{r.type, r.name, r.level, r.meta, Prim::None, {}});
        LinkTree(t);
        return t;
    }();
    return tree;
}

bool IsByteElement(const TreeNode& n) {
    return n.prim == Prim::U8 || n.prim == Prim::S8;
}

bool ReadValue(const Tree& t, int index, Cursor& c, Value& out, int depth, std::size_t& budget) {
    if (depth > MaxDepth || !c.ok || budget == 0)
        return false;
    --budget;
    const TreeNode& n = t.nodes[static_cast<std::size_t>(index)];
    bool align = (n.meta & 0x4000) != 0;
    switch (n.prim) {
    case Prim::S8:
        out.kind = Value::Kind::Int;
        out.i = c.Int<std::int8_t>();
        break;
    case Prim::U8:
        out.kind = Value::Kind::UInt;
        out.u = c.Int<u8>();
        break;
    case Prim::S16:
        out.kind = Value::Kind::Int;
        out.i = c.Int<std::int16_t>();
        break;
    case Prim::U16:
        out.kind = Value::Kind::UInt;
        out.u = c.Int<u16>();
        break;
    case Prim::S32:
        out.kind = Value::Kind::Int;
        out.i = c.Int<i32>();
        break;
    case Prim::U32:
        out.kind = Value::Kind::UInt;
        out.u = c.Int<u32>();
        break;
    case Prim::S64:
        out.kind = Value::Kind::Int;
        out.i = c.Int<i64>();
        break;
    case Prim::U64:
        out.kind = Value::Kind::UInt;
        out.u = c.Int<u64>();
        break;
    case Prim::F32:
        out.kind = Value::Kind::Float;
        out.f = c.F32();
        break;
    case Prim::F64:
        out.kind = Value::Kind::Float;
        out.f = c.F64();
        break;
    case Prim::Bool:
        out.kind = Value::Kind::Bool;
        out.i = c.Int<u8>() != 0;
        break;
    case Prim::String: {
        const i32 len = c.Int<i32>();
        if (len < 0 || !c.Bytes(out.s, static_cast<std::size_t>(len)))
            return false;
        out.kind = Value::Kind::String;
        c.Align();
        break;
    }
    case Prim::Typeless: {
        const i32 len = c.Int<i32>();
        if (len < 0 || !c.Bytes(out.s, static_cast<std::size_t>(len)))
            return false;
        out.kind = Value::Kind::Bytes;
        break;
    }
    case Prim::None:
        if (n.type == "pair" && n.children.size() == 2) {
            out.kind = Value::Kind::Array;
            out.items.resize(2);
            if (!ReadValue(t, n.children[0], c, out.items[0], depth + 1, budget) ||
                !ReadValue(t, n.children[1], c, out.items[1], depth + 1, budget))
                return false;
        } else if (!n.children.empty() &&
                   t.nodes[static_cast<std::size_t>(n.children[0])].type == "Array") {
            const TreeNode& arr = t.nodes[static_cast<std::size_t>(n.children[0])];
            if (arr.meta & 0x4000)
                align = true;
            if (arr.children.size() < 2)
                return false;
            const i32 count = c.Int<i32>();
            const int sub = arr.children[1];
            const TreeNode& subnode = t.nodes[static_cast<std::size_t>(sub)];
            // a Value element costs sizeof(Value) up front: the count is capped by MaxAlloc too, not
            // only by the bytes left and the value budget (4M values would be ~0.5 GiB)
            if (!c.ok || count < 0 || static_cast<std::size_t>(count) > c.Remaining() ||
                (!IsByteElement(subnode) && (static_cast<std::size_t>(count) > budget ||
                                             static_cast<std::size_t>(count) > MaxAlloc / sizeof(Value))))
                return false;
            if (IsByteElement(subnode)) {
                if (!c.Bytes(out.s, static_cast<std::size_t>(count)))
                    return false;
                out.kind = Value::Kind::Bytes;
                if (subnode.meta & 0x4000)
                    c.Align();
            } else {
                out.kind = Value::Kind::Array;
                out.items.resize(static_cast<std::size_t>(count));
                for (auto& item : out.items)
                    if (!ReadValue(t, sub, c, item, depth + 1, budget))
                        return false;
            }
        } else {
            out.kind = Value::Kind::Object;
            out.fields.resize(n.children.size());
            for (std::size_t k = 0; k < n.children.size(); ++k) {
                out.fields[k].first = t.nodes[static_cast<std::size_t>(n.children[k])].name;
                if (!ReadValue(t, n.children[k], c, out.fields[k].second, depth + 1, budget))
                    return false;
            }
        }
        break;
    }
    if (align)
        c.Align();
    return c.ok;
}

// ---- small value helpers ---------------------------------------------------------------------

float F(const Value* v) {
    return v ? static_cast<float>(v->AsFloat()) : 0.0f;
}

PPtr PtrOf(const Value* v) {
    PPtr p;
    if (v) {
        if (const Value* f = v->Get("m_FileID"))
            p.file_id = static_cast<i32>(f->AsInt());
        if (const Value* f = v->Get("m_PathID"))
            p.path_id = f->AsInt();
    }
    return p;
}

Rectf RectOf(const Value* v) {
    Rectf r;
    if (v) {
        r.x = F(v->Get("x"));
        r.y = F(v->Get("y"));
        r.width = F(v->Get("width"));
        r.height = F(v->Get("height"));
    }
    return r;
}

void GuidOf(const Value* v, u8 out[16]) {
    std::memset(out, 0, 16);
    if (!v || v->kind != Value::Kind::Object)
        return;
    for (std::size_t k = 0; k < 4 && k < v->fields.size(); ++k) {
        const u32 w = static_cast<u32>(v->fields[k].second.AsInt());
        for (int b = 0; b < 4; ++b)
            out[k * 4 + b] = static_cast<u8>(w >> (8 * b));
    }
}

std::string Basename(std::string_view p) {
    const auto slash = p.find_last_of("/\\");
    return std::string{slash == std::string_view::npos ? p : p.substr(slash + 1)};
}

int CompareVersion(std::string_view v, int a, int b, int c) {
    int parts[3]{};
    std::size_t k = 0;
    for (std::size_t i = 0; i < v.size() && k < 3;) {
        if (v[i] < '0' || v[i] > '9')
            break;
        std::size_t j = i;
        while (j < v.size() && v[j] >= '0' && v[j] <= '9')
            ++j;
        // a part too long for an int is newer than any wanted version (no signed overflow)
        parts[k++] = ce_parse::Index(v.substr(i, j - i)).value_or(std::numeric_limits<int>::max());
        i = j;
        if (i < v.size() && v[i] == '.')
            ++i;
        else
            break;
    }
    const int want[3]{a, b, c};
    for (int i = 0; i < 3; ++i)
        if (parts[i] != want[i])
            return parts[i] < want[i] ? -1 : 1;
    return 0;
}

} // namespace

// ---- Value -------------------------------------------------------------------------------------

const Value* Value::Get(std::string_view name) const {
    if (kind == Kind::Object) {
        for (const auto& [k, v] : fields)
            if (k == name)
                return &v;
    } else if (kind == Kind::Array && items.size() == 2) {
        if (name == "first")
            return &items[0];
        if (name == "second")
            return &items[1];
    }
    return nullptr;
}

const Value* Value::Path(std::string_view dotted) const {
    const Value* v = this;
    while (v && !dotted.empty()) {
        const auto dot = dotted.find('.');
        v = v->Get(dotted.substr(0, dot));
        dotted = dot == std::string_view::npos ? std::string_view{} : dotted.substr(dot + 1);
    }
    return v;
}

std::int64_t Value::AsInt(std::int64_t fallback) const {
    switch (kind) {
    case Kind::Int:
    case Kind::Bool:
        return i;
    case Kind::UInt:
        return static_cast<std::int64_t>(u);
    case Kind::Float:
        return static_cast<std::int64_t>(f);
    default:
        return fallback;
    }
}

double Value::AsFloat(double fallback) const {
    switch (kind) {
    case Kind::Float:
        return f;
    case Kind::Int:
    case Kind::Bool:
        return static_cast<double>(i);
    case Kind::UInt:
        return static_cast<double>(u);
    default:
        return fallback;
    }
}

std::string_view Value::AsString() const {
    return kind == Kind::String || kind == Kind::Bytes ? std::string_view{s} : std::string_view{};
}

// ---- readers -----------------------------------------------------------------------------------

RangeReader MakeRangeReader(RomfsReader whole) {
    struct State {
        RomfsReader read;
        std::mutex mutex;
        std::string path;
        std::shared_ptr<const std::vector<u8>> bytes;
        std::shared_ptr<const std::vector<u8>> Get(std::string_view p) {
            std::lock_guard lock{mutex};
            if (bytes && path == p)
                return bytes;
            auto data = read ? read(p) : std::nullopt;
            if (!data)
                return nullptr;
            path = std::string{p};
            bytes = std::make_shared<const std::vector<u8>>(std::move(*data));
            return bytes;
        }
    };
    auto state = std::make_shared<State>();
    state->read = std::move(whole);
    RangeReader r;
    r.size = [state](std::string_view p) -> std::optional<u64> {
        try {
            const auto b = state->Get(p);
            if (!b)
                return std::nullopt;
            return b->size();
        } catch (...) {
            return std::nullopt;
        }
    };
    r.read = [state](std::string_view p, u64 off, u8* out, std::size_t n) {
        try {
            const auto b = state->Get(p);
            if (!b || off > b->size() || n > b->size() - off)
                return false;
            if (n)
                std::memcpy(out, b->data() + off, n);
            return true;
        } catch (...) {
            return false;
        }
    };
    return r;
}

AstcDecoder MakeHostAstcDecoder(void* userdata, EdenDsmodAstcDecoder decode) {
    if (!decode)
        return {};
    return [userdata, decode](const u8* blocks, std::size_t size, u32 w, u32 h, u32 bw, u32 bh,
                              std::vector<u8>& rgba) {
        if (w == 0 || h == 0 || static_cast<u64>(w) * h * 4 > MaxAlloc)
            return false;
        rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
        return decode(userdata, w, h, bw, bh, blocks, size, rgba.data(), rgba.size()) != 0;
    };
}

// ---- Bundle ------------------------------------------------------------------------------------

struct Bundle::Impl {
    struct Block {
        u64 uoff{}, coff{};
        u32 usize{}, csize{};
        u16 flags{};
    };
    ByteSource file;
    std::vector<Block> blocks;
    std::vector<BundleNode> nodes;
    u64 total{};
    mutable std::mutex mutex;
    mutable std::list<std::pair<std::size_t, std::shared_ptr<const std::vector<u8>>>> cache;
    mutable std::size_t cached_bytes{};

    std::shared_ptr<const std::vector<u8>> BlockData(std::size_t index) const {
        for (auto it = cache.begin(); it != cache.end(); ++it) {
            if (it->first == index) {
                cache.splice(cache.begin(), cache, it);
                return cache.front().second;
            }
        }
        const Block& b = blocks[index];
        std::vector<u8> raw(b.csize);
        if (!file.read(b.coff, raw.data(), raw.size()))
            return nullptr;
        auto out = std::make_shared<std::vector<u8>>();
        switch (b.flags & 0x3F) {
        case 0:
            if (b.csize != b.usize)
                return nullptr;
            *out = std::move(raw);
            break;
        case 2:
        case 3: {
            out->resize(b.usize);
            const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(raw.data()),
                                              reinterpret_cast<char*>(out->data()),
                                              static_cast<int>(b.csize), static_cast<int>(b.usize));
            if (n != static_cast<int>(b.usize))
                return nullptr;
            break;
        }
        default:
            return nullptr; // LZMA / LZHAM: not used by this game
        }
        Count(g_stats.blocks_decompressed);
        Count(g_stats.bytes_decompressed, out->size());
        cache.emplace_front(index, out);
        cached_bytes += out->size();
        const std::size_t budget = g_block_budget.load(std::memory_order_relaxed);
        while (cache.size() > 1 && cached_bytes > budget) {
            cached_bytes -= cache.back().second->size();
            cache.pop_back();
        }
        return out;
    }
};

Bundle::Bundle() : impl(std::make_unique<Impl>()) {}
Bundle::~Bundle() = default;

std::shared_ptr<Bundle> Bundle::Open(ByteSource file) {
    try {
        if (!file.read || file.size < 48)
            return nullptr;
        file.read = [inner = std::move(file.read)](u64 off, u8* out, std::size_t n) {
            Count(g_stats.romfs_reads);
            Count(g_stats.romfs_bytes, n);
            return inner(off, out, n);
        };
        std::vector<u8> head(static_cast<std::size_t>(std::min<u64>(file.size, 512)));
        if (!file.read(0, head.data(), head.size()))
            return nullptr;
        Cursor c{head.data(), head.size(), 0, 0, true};
        if (c.CStr() != "UnityFS")
            return nullptr;
        std::shared_ptr<Bundle> b{new Bundle()};
        b->format = c.Int<u32>();
        c.CStr(); // player version "5.x.x"
        b->engine_version = c.CStr();
        const u64 size = c.Int<u64>();
        const u32 csize = c.Int<u32>();
        const u32 usize = c.Int<u32>();
        const u32 flags = c.Int<u32>();
        b->header_flags = flags;
        if (!c.ok || b->format < 6 || b->format > 8 || size > file.size || csize > MaxAlloc ||
            usize > MaxAlloc)
            return nullptr;
        const std::string_view ev = b->engine_version;
        const bool new_flags = CompareVersion(ev, 2020, 3, 34) >= 0 &&
                               !(CompareVersion(ev, 2021, 0, 0) >= 0 && CompareVersion(ev, 2021, 3, 2) < 0) &&
                               !(CompareVersion(ev, 2022, 0, 0) >= 0 && CompareVersion(ev, 2022, 1, 1) < 0);
        if (!new_flags && (flags & 0x200))
            return nullptr; // Unity CN encryption (old flag set)
        u64 pos = c.pos;
        if (b->format >= 7 || CompareVersion(ev, 2019, 4, 15) >= 0)
            pos = (pos + 15) / 16 * 16;
        std::vector<u8> info_raw(csize);
        u64 data_start;
        if (flags & 0x80) {
            if (csize > file.size)
                return nullptr;
            if (!file.read(file.size - csize, info_raw.data(), csize))
                return nullptr;
            data_start = pos;
        } else {
            if (pos + csize > file.size || !file.read(pos, info_raw.data(), csize))
                return nullptr;
            data_start = pos + csize;
        }
        std::vector<u8> info;
        switch (flags & 0x3F) {
        case 0:
            info = std::move(info_raw);
            break;
        case 2:
        case 3: {
            info.resize(usize);
            const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(info_raw.data()),
                                              reinterpret_cast<char*>(info.data()),
                                              static_cast<int>(csize), static_cast<int>(usize));
            if (n != static_cast<int>(usize))
                return nullptr;
            break;
        }
        default:
            return nullptr;
        }
        if (new_flags && (flags & 0x200))
            data_start = (data_start + 15) / 16 * 16;
        Cursor bi{info.data(), info.size(), 0, 0, true};
        bi.Skip(16); // uncompressed data hash
        const i32 nblocks = bi.Int<i32>();
        if (!bi.ok || nblocks < 0 || static_cast<std::size_t>(nblocks) * 10 > bi.Remaining())
            return nullptr;
        u64 uoff = 0, coff = data_start;
        for (i32 k = 0; k < nblocks; ++k) {
            Impl::Block blk;
            blk.usize = bi.Int<u32>();
            blk.csize = bi.Int<u32>();
            blk.flags = bi.Int<u16>();
            // Stored blocks are read in place (the Luminescent texturemass is one 110 MB block);
            // compressed ones are decompressed whole, so they are capped.
            if ((blk.flags & 0x3F) == 0 ? blk.usize != blk.csize
                                        : blk.usize > MaxAlloc || blk.csize > MaxAlloc)
                return nullptr;
            blk.uoff = uoff;
            blk.coff = coff;
            uoff += blk.usize;
            coff += blk.csize;
            if (coff > file.size)
                return nullptr;
            b->impl->blocks.push_back(blk);
        }
        b->impl->total = uoff;
        const i32 nnodes = bi.Int<i32>();
        if (!bi.ok || nnodes < 0 || nnodes > 4096)
            return nullptr;
        for (i32 k = 0; k < nnodes; ++k) {
            BundleNode n;
            n.offset = bi.Int<u64>();
            n.size = bi.Int<u64>();
            n.flags = bi.Int<u32>();
            n.path = bi.CStr();
            if (!bi.ok || n.offset > uoff || n.size > uoff - n.offset)
                return nullptr;
            b->impl->nodes.push_back(std::move(n));
        }
        b->impl->file = std::move(file);
        Count(g_stats.bundles_opened);
        return b;
    } catch (...) {
        return nullptr;
    }
}

std::vector<std::array<std::uint64_t, 3>> Bundle::BlockSummary() const {
    std::map<u32, std::array<u64, 3>> m;
    for (const auto& b : impl->blocks) {
        auto& e = m[b.flags & 0x3Fu];
        e[0] = b.flags & 0x3Fu;
        e[1] += 1;
        e[2] += b.usize;
    }
    std::vector<std::array<u64, 3>> out;
    for (const auto& [k, v] : m)
        out.push_back(v);
    return out;
}

const std::vector<BundleNode>& Bundle::Nodes() const {
    return impl->nodes;
}

const BundleNode* Bundle::FindNode(std::string_view name) const {
    for (const auto& n : impl->nodes)
        if (n.path == name)
            return &n;
    const std::string base = Basename(name);
    for (const auto& n : impl->nodes)
        if (Basename(n.path) == base)
            return &n;
    return nullptr;
}

bool Bundle::Read(std::uint64_t offset, std::uint8_t* out, std::size_t size) const {
    try {
        if (offset > impl->total || size > impl->total - offset)
            return false;
        std::lock_guard lock{impl->mutex};
        const auto& blocks = impl->blocks;
        auto it = std::upper_bound(blocks.begin(), blocks.end(), offset,
                                   [](u64 v, const Impl::Block& b) { return v < b.uoff; });
        if (it == blocks.begin())
            return size == 0;
        std::size_t index = static_cast<std::size_t>(it - blocks.begin()) - 1;
        while (size > 0) {
            if (index >= blocks.size())
                return false;
            const Impl::Block& b = blocks[index];
            const u64 in = offset - b.uoff;
            const std::size_t n = static_cast<std::size_t>(std::min<u64>(size, b.usize - in));
            if ((b.flags & 0x3F) == 0) {
                if (!impl->file.read(b.coff + in, out, n))
                    return false;
            } else {
                const auto data = impl->BlockData(index);
                if (!data)
                    return false;
                std::memcpy(out, data->data() + in, n);
            }
            out += n;
            offset += n;
            size -= n;
            ++index;
        }
        return true;
    } catch (...) {
        return false;
    }
}

ByteSource Bundle::NodeSource(const BundleNode& node) const {
    ByteSource s;
    s.size = node.size;
    const u64 base = node.offset;
    const u64 limit = node.size;
    std::shared_ptr<const Bundle> self = shared_from_this();
    // Small nodes (the CAB serialized files: tables + object data) are loaded once, so indexing
    // thousands of objects does not go back to the romfs per object.
    // (lp_unity preloads up to 8 MiB; Chained Echoes scene bundles hold ~40 such nodes each, so
    // only small ones are preloaded and the rest go through the bounded block cache.)
    constexpr u64 Preload = 1u * 1024u * 1024u;
    if (limit <= Preload) {
        auto bytes = std::make_shared<std::vector<u8>>(static_cast<std::size_t>(limit));
        if (Read(base, bytes->data(), bytes->size())) {
            s.read = [bytes](u64 off, u8* out, std::size_t n) {
                if (off > bytes->size() || n > bytes->size() - off)
                    return false;
                if (n)
                    std::memcpy(out, bytes->data() + off, n);
                return true;
            };
            return s;
        }
    }
    s.read = [self, base, limit](u64 off, u8* out, std::size_t n) {
        if (off > limit || n > limit - off)
            return false;
        return self->Read(base + off, out, n);
    };
    return s;
}

// ---- SerializedFile ----------------------------------------------------------------------------

struct SerializedFile::Impl {
    ByteSource file;
    u32 version{};
    bool big{};
    std::string unity_version;
    i32 platform{};
    bool type_trees{};
    std::vector<i32> type_class;
    std::vector<std::optional<Tree>> trees;
    std::vector<ObjectInfo> objects;
    std::vector<std::pair<i64, u32>> by_id; // sorted by path id (16 B/object; mapstextures has 153k)
};

SerializedFile::SerializedFile() : impl(std::make_unique<Impl>()) {}
SerializedFile::~SerializedFile() = default;

std::shared_ptr<SerializedFile> SerializedFile::Open(ByteSource file) {
    try {
        if (!file.read || file.size < 20)
            return nullptr;
        u8 head[48]{};
        const std::size_t hn = static_cast<std::size_t>(std::min<u64>(file.size, 48));
        if (!file.read(0, head, hn))
            return nullptr;
        Cursor h{head, hn, 0, 0, true};
        u64 metadata_size = h.Int<u32>();
        h.Int<u32>(); // file size
        const u32 version = h.Int<u32>();
        u64 data_offset = h.Int<u32>();
        if (version < 17 || version > 22)
            return nullptr;
        const bool big = h.Int<u8>() != 0;
        h.Skip(3);
        if (version >= 22) {
            metadata_size = h.Int<u32>();
            h.Int<u64>(); // file size
            data_offset = h.Int<u64>();
            h.Int<u64>();
        }
        const std::size_t meta_start = h.pos;
        if (!h.ok || metadata_size > MaxAlloc || meta_start + metadata_size > file.size ||
            data_offset > file.size)
            return nullptr;
        std::vector<u8> meta(meta_start + static_cast<std::size_t>(metadata_size));
        if (!file.read(0, meta.data(), meta.size()))
            return nullptr;
        Cursor c{meta.data(), meta.size(), meta_start, 0, big};
        std::shared_ptr<SerializedFile> sf{new SerializedFile()};
        Impl& m = *sf->impl;
        m.version = version;
        m.big = big;
        m.unity_version = c.CStr();
        m.platform = c.Int<i32>();
        m.type_trees = c.Int<u8>() != 0;
        const i32 ntypes = c.Int<i32>();
        if (!c.ok || ntypes < 0 || ntypes > 65536)
            return nullptr;
        for (i32 k = 0; k < ntypes; ++k) {
            const i32 class_id = c.Int<i32>();
            c.Int<u8>(); // stripped
            c.Int<std::int16_t>(); // script type index
            if (class_id == 114)
                c.Skip(16); // script id
            c.Skip(16); // old type hash
            std::optional<Tree> tree;
            if (m.type_trees) {
                tree = ParseTreeBlob(c, version);
                if (!tree)
                    return nullptr;
                if (version >= 21) {
                    const i32 deps = c.Int<i32>();
                    if (deps < 0 || static_cast<std::size_t>(deps) * 4 > c.Remaining())
                        return nullptr;
                    c.Skip(static_cast<std::size_t>(deps) * 4);
                }
            }
            if (!c.ok)
                return nullptr;
            m.type_class.push_back(class_id);
            m.trees.push_back(std::move(tree));
        }
        const i32 nobjects = c.Int<i32>();
        if (!c.ok || nobjects < 0 || static_cast<std::size_t>(nobjects) * 20 > c.Remaining())
            return nullptr;
        m.objects.reserve(static_cast<std::size_t>(nobjects));
        for (i32 k = 0; k < nobjects; ++k) {
            c.Align();
            ObjectInfo o;
            o.path_id = c.Int<i64>();
            o.byte_start = (version >= 22 ? c.Int<u64>() : c.Int<u32>()) + data_offset;
            o.byte_size = c.Int<u32>();
            o.type_index = c.Int<i32>();
            if (!c.ok || o.type_index < 0 || o.type_index >= ntypes ||
                o.byte_start > file.size || o.byte_size > file.size - o.byte_start)
                return nullptr;
            o.class_id = m.type_class[static_cast<std::size_t>(o.type_index)];
            m.by_id.emplace_back(o.path_id, static_cast<u32>(m.objects.size()));
            m.objects.push_back(o);
        }
        const i32 nscripts = c.Int<i32>();
        if (!c.ok || nscripts < 0 || static_cast<std::size_t>(nscripts) > c.Remaining())
            return nullptr;
        for (i32 k = 0; k < nscripts; ++k) {
            c.Int<i32>();
            c.Align();
            c.Int<i64>();
        }
        std::stable_sort(m.by_id.begin(), m.by_id.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        m.file = std::move(file);
        return sf;
    } catch (...) {
        return nullptr;
    }
}

std::uint32_t SerializedFile::Version() const {
    return impl->version;
}
bool SerializedFile::HasTypeTrees() const {
    return impl->type_trees;
}
std::int32_t SerializedFile::Platform() const {
    return impl->platform;
}
const std::vector<ObjectInfo>& SerializedFile::Objects() const {
    return impl->objects;
}
const ObjectInfo* SerializedFile::Find(std::int64_t path_id) const {
    const auto it = std::lower_bound(impl->by_id.begin(), impl->by_id.end(), path_id,
                                     [](const auto& e, i64 v) { return e.first < v; });
    return it == impl->by_id.end() || it->first != path_id ? nullptr : &impl->objects[it->second];
}

std::optional<std::vector<std::uint8_t>> SerializedFile::ReadRaw(const ObjectInfo& obj) const {
    try {
        if (obj.byte_size > MaxAlloc)
            return std::nullopt;
        std::vector<u8> out(obj.byte_size);
        if (!impl->file.read(obj.byte_start, out.data(), out.size()))
            return std::nullopt;
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

namespace {
const Tree* TreeFor(const std::vector<std::optional<Tree>>& trees, std::string_view unity_version,
                    const ObjectInfo& obj) {
    const auto ti = static_cast<std::size_t>(obj.type_index);
    if (ti < trees.size() && trees[ti])
        return &*trees[ti];
    if (obj.class_id == ClassFont && CompareVersion(unity_version, 2019, 0, 0) >= 0 &&
        CompareVersion(unity_version, 2020, 0, 0) < 0)
        return &BuiltinFontTree();
    return nullptr;
}
} // namespace

std::optional<Value> SerializedFile::ReadObject(const ObjectInfo& obj) const {
    try {
        const Tree* tree = TreeFor(impl->trees, impl->unity_version, obj);
        if (!tree)
            return std::nullopt;
        const auto raw = ReadRaw(obj);
        if (!raw)
            return std::nullopt;
        Cursor c{raw->data(), raw->size(), 0, 0, impl->big};
        Value v;
        std::size_t budget = MaxValues;
        if (!ReadValue(*tree, 0, c, v, 0, budget))
            return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> SerializedFile::PeekName(const ObjectInfo& obj) const {
    try {
        // Where m_Name starts: walk the typetree's leading fields over a prefix of the object.
        u64 name_at = 0;
        const Tree* tree = TreeFor(impl->trees, impl->unity_version, obj);
        if (tree) {
            const TreeNode& root = tree->nodes[0];
            std::size_t k = 0;
            while (k < root.children.size() &&
                   tree->nodes[static_cast<std::size_t>(root.children[k])].name != "m_Name")
                ++k;
            if (k == root.children.size())
                return std::nullopt;
            if (k > 0) {
                std::vector<u8> prefix(std::min<std::size_t>(obj.byte_size, 4096));
                if (!impl->file.read(obj.byte_start, prefix.data(), prefix.size()))
                    return std::nullopt;
                Cursor c{prefix.data(), prefix.size(), 0, 0, impl->big};
                std::size_t budget = 4096;
                for (std::size_t f = 0; f < k; ++f) {
                    Value skip;
                    if (!ReadValue(*tree, root.children[f], c, skip, 1, budget))
                        return std::nullopt;
                }
                name_at = c.pos;
            }
        } else if (obj.class_id == 114) {
            name_at = 28; // m_GameObject PPtr, m_Enabled (aligned), m_Script PPtr
        }
        if (obj.byte_size < 4 || name_at > obj.byte_size - 4)
            return std::nullopt;
        u8 len_bytes[4];
        if (!impl->file.read(obj.byte_start + name_at, len_bytes, 4))
            return std::nullopt;
        Cursor c{len_bytes, 4, 0, 0, impl->big};
        const i32 len = c.Int<i32>();
        if (len < 0 || len > 4096 || static_cast<u64>(len) > obj.byte_size - 4 - name_at)
            return std::nullopt;
        std::string s(static_cast<std::size_t>(len), '\0');
        if (len && !impl->file.read(obj.byte_start + name_at + 4, reinterpret_cast<u8*>(s.data()),
                                    s.size()))
            return std::nullopt;
        return s;
    } catch (...) {
        return std::nullopt;
    }
}

// ---- textures ----------------------------------------------------------------------------------

namespace {

enum class Codec : u8 { Alpha8, RGB24, RGBA32, ARGB32, BGRA32, R8, Dxt1, Dxt5, Bc4, Bc5, Bc7, Astc };

struct FormatDesc {
    Codec codec;
    u32 bw, bh, bpb; // block size in pixels, bytes per block (pixel formats: 1x1, bytes/pixel)
    u32 sw, sh;      // UnityPy TEXTURE_FORMAT_BLOCK_SIZE_MAP (texels per 16 swizzle bytes)
};

std::optional<FormatDesc> Describe(i32 format) {
    switch (format) {
    case 1:
        return FormatDesc{Codec::Alpha8, 1, 1, 1, 16, 1};
    case 3:
        return FormatDesc{Codec::RGB24, 1, 1, 3, 0, 0}; // swizzled RGB24 is stored as RGBA32
    case 4:
        return FormatDesc{Codec::RGBA32, 1, 1, 4, 4, 1};
    case 5:
        return FormatDesc{Codec::ARGB32, 1, 1, 4, 4, 1};
    case 14:
        return FormatDesc{Codec::BGRA32, 1, 1, 4, 4, 1};
    case 63:
        return FormatDesc{Codec::R8, 1, 1, 1, 16, 1};
    case 10:
        return FormatDesc{Codec::Dxt1, 4, 4, 8, 8, 4};
    case 12:
        return FormatDesc{Codec::Dxt5, 4, 4, 16, 4, 4};
    case 26:
        return FormatDesc{Codec::Bc4, 4, 4, 8, 8, 4};
    case 27:
        return FormatDesc{Codec::Bc5, 4, 4, 16, 4, 4};
    case 25:
        return FormatDesc{Codec::Bc7, 4, 4, 16, 4, 4};
    default:
        break;
    }
    if ((format >= 48 && format <= 59) || (format >= 66 && format <= 71)) {
        static constexpr u32 sizes[6]{4, 5, 6, 8, 10, 12};
        const int base = format >= 66 ? 66 : format >= 54 ? 54 : 48;
        const u32 b = sizes[format - base];
        if (format >= 66)
            return std::nullopt; // HDR profile: not used by this game
        return FormatDesc{Codec::Astc, b, b, 16, b, b};
    }
    return std::nullopt;
}

u32 CeilDiv(u32 a, u32 b) {
    return (a + b - 1) / b;
}

struct SwitchInfo {
    bool swizzled{};
    u32 gobs{};
};

SwitchInfo SwitchOf(const TextureInfo& tex, i32 platform) {
    SwitchInfo s;
    if (platform != PlatformSwitch || tex.platform_blob.size() < 12)
        return s;
    const u8* b = tex.platform_blob.data() + 8;
    const u32 shift = u32{b[0]} | u32{b[1]} << 8 | u32{b[2]} << 16 | u32{b[3]} << 24;
    if (shift >= 16)
        return s;
    s.gobs = 1u << shift;
    s.swizzled = s.gobs > 1;
    return s;
}

/// Decodes a grid of nbx x nby blocks (row-major) into RGBA8 of (nbx*bw) x (nby*bh).
bool DecodeBlocks(const FormatDesc& f, const u8* src, std::size_t src_size, u32 nbx, u32 nby,
                  const AstcDecoder& astc, std::vector<u8>& rgba) {
    const u32 w = nbx * f.bw, h = nby * f.bh;
    if (static_cast<u64>(w) * h * 4 > MaxAlloc ||
        static_cast<u64>(nbx) * nby * f.bpb > src_size)
        return false;
    rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
    switch (f.codec) {
    case Codec::Astc: {
        std::vector<u8> out;
        if (!astc || !astc(src, static_cast<std::size_t>(nbx) * nby * 16, w, h, f.bw, f.bh, out) ||
            out.size() < rgba.size())
            return false;
        std::memcpy(rgba.data(), out.data(), rgba.size());
        return true;
    }
    case Codec::Dxt1:
    case Codec::Dxt5:
    case Codec::Bc7:
        for (u32 by = 0; by < nby; ++by)
            for (u32 bx = 0; bx < nbx; ++bx) {
                const u8* s = src + (static_cast<std::size_t>(by) * nbx + bx) * f.bpb;
                u8* o = rgba.data() + (static_cast<std::size_t>(by) * 4 * w + bx * 4) * 4;
                if (f.codec == Codec::Dxt1)
                    bcn::DecodeBc1(s, o, bx * 4, by * 4, w, h);
                else if (f.codec == Codec::Dxt5)
                    bcn::DecodeBc3(s, o, bx * 4, by * 4, w, h);
                else
                    bcn::DecodeBc7(s, o, bx * 4, by * 4, w, h);
            }
        return true;
    case Codec::Bc4:
    case Codec::Bc5: {
        const u32 ch = f.codec == Codec::Bc4 ? 1 : 2;
        std::vector<u8> plane(static_cast<std::size_t>(w) * h * ch);
        for (u32 by = 0; by < nby; ++by)
            for (u32 bx = 0; bx < nbx; ++bx) {
                const u8* s = src + (static_cast<std::size_t>(by) * nbx + bx) * f.bpb;
                u8* o = plane.data() + (static_cast<std::size_t>(by) * 4 * w + bx * 4) * ch;
                if (ch == 1)
                    bcn::DecodeBc4(s, o, bx * 4, by * 4, w, h, false);
                else
                    bcn::DecodeBc5(s, o, bx * 4, by * 4, w, h, false);
            }
        for (std::size_t p = 0; p < static_cast<std::size_t>(w) * h; ++p) {
            u8* q = rgba.data() + p * 4;
            if (ch == 1) {
                q[0] = q[1] = q[2] = plane[p]; // Pillow "L"
            } else {
                q[0] = plane[p * 2];
                q[1] = plane[p * 2 + 1];
                q[2] = 0;
            }
            q[3] = 0xFF;
        }
        return true;
    }
    default:
        break;
    }
    // Pixel formats (1x1 blocks).
    for (std::size_t p = 0; p < static_cast<std::size_t>(w) * h; ++p) {
        const u8* s = src + p * f.bpb;
        u8* q = rgba.data() + p * 4;
        switch (f.codec) {
        case Codec::Alpha8:
            q[0] = q[1] = q[2] = 0; // Pillow RGBA <- raw "A": colour channels stay zero
            q[3] = s[0];
            break;
        case Codec::RGB24:
            q[0] = s[0], q[1] = s[1], q[2] = s[2], q[3] = 0xFF;
            break;
        case Codec::RGBA32:
            std::memcpy(q, s, 4);
            break;
        case Codec::ARGB32:
            q[0] = s[1], q[1] = s[2], q[2] = s[3], q[3] = s[0];
            break;
        case Codec::BGRA32:
            q[0] = s[2], q[1] = s[1], q[2] = s[0], q[3] = s[3];
            break;
        case Codec::R8:
            q[0] = s[0], q[1] = 0, q[2] = 0, q[3] = 0xFF;
            break;
        default:
            return false;
        }
    }
    return true;
}

/// Fetches `size` bytes of mip 0 at `offset` (relative to the texture's image data).
using DataFetch = std::function<bool(u64 offset, u8* out, std::size_t size)>;

/// Decodes the raw-orientation (Unity bottom-up row order, as UnityPy holds it before its
/// final flip) region [x0,x1) x [y0,y1) of the texture. Pixels outside the texture are 0.
std::optional<Image> DecodeRegion(const DataFetch& fetch, u64 data_size, const TextureInfo& tex,
                                  i32 platform, const AstcDecoder& astc, i64 x0, i64 y0, i64 x1,
                                  i64 y1) {
    if (tex.width <= 0 || tex.height <= 0 || static_cast<u32>(tex.width) > MaxDim ||
        static_cast<u32>(tex.height) > MaxDim || x1 < x0 || y1 < y0 || x1 - x0 > MaxDim ||
        y1 - y0 > MaxDim)
        return std::nullopt;
    auto desc = Describe(tex.format);
    if (!desc)
        return std::nullopt;
    const u32 w = static_cast<u32>(tex.width), h = static_cast<u32>(tex.height);
    const SwitchInfo sw = SwitchOf(tex, platform);
    // Data layout dimensions (padded) as UnityPy decodes them.
    u32 dw, dh;
    std::vector<u8> linear; // whole linear data when it had to be deswizzled
    if (sw.swizzled) {
        if (desc->codec == Codec::RGB24)
            desc = Describe(4); // UnityPy: swizzled RGB24 is RGBA32
        if (!desc || desc->sw == 0)
            return std::nullopt;
        dw = CeilDiv(w, desc->sw * 4) * desc->sw * 4;
        dh = CeilDiv(h, desc->sh * 8 * sw.gobs) * desc->sh * 8 * sw.gobs;
        const u64 bytes = static_cast<u64>(CeilDiv(dw, desc->sw)) * CeilDiv(dh, desc->sh) * 16;
        if (bytes > MaxAlloc || bytes > data_size)
            return std::nullopt;
        std::vector<u8> raw(static_cast<std::size_t>(bytes));
        if (!fetch(0, raw.data(), raw.size()))
            return std::nullopt;
        linear = SwitchDeswizzle(raw, dw, dh, desc->sw, desc->sh, sw.gobs);
        if (linear.size() != raw.size())
            return std::nullopt;
    } else {
        dw = CeilDiv(w, desc->bw) * desc->bw;
        dh = CeilDiv(h, desc->bh) * desc->bh;
    }
    const u32 stride_blocks = CeilDiv(dw, desc->bw);
    const u64 row_bytes = static_cast<u64>(stride_blocks) * desc->bpb;
    const u64 needed = row_bytes * CeilDiv(dh, desc->bh);
    if (!sw.swizzled && needed > data_size)
        return std::nullopt;

    Image out;
    out.width = static_cast<u32>(x1 - x0);
    out.height = static_cast<u32>(y1 - y0);
    if (static_cast<u64>(out.width) * out.height * 4 > MaxAlloc)
        return std::nullopt;
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
    // Clip to the original (cropped) texture, as UnityPy crops to m_Width x m_Height first.
    const i64 cx0 = std::max<i64>(x0, 0), cy0 = std::max<i64>(y0, 0);
    const i64 cx1 = std::min<i64>(x1, w), cy1 = std::min<i64>(y1, h);
    if (cx0 >= cx1 || cy0 >= cy1)
        return out;
    const u32 bx0 = static_cast<u32>(cx0) / desc->bw, bx1 = CeilDiv(static_cast<u32>(cx1), desc->bw);
    const u32 by0 = static_cast<u32>(cy0) / desc->bh, by1 = CeilDiv(static_cast<u32>(cy1), desc->bh);
    const u32 nbx = bx1 - bx0, nby = by1 - by0;
    std::vector<u8> rows(static_cast<std::size_t>(row_bytes * nby));
    if (sw.swizzled) {
        std::memcpy(rows.data(), linear.data() + by0 * row_bytes, rows.size());
    } else if (!fetch(by0 * row_bytes, rows.data(), rows.size())) {
        return std::nullopt;
    }
    std::vector<u8> grid(static_cast<std::size_t>(nbx) * nby * desc->bpb);
    for (u32 r = 0; r < nby; ++r)
        std::memcpy(grid.data() + static_cast<std::size_t>(r) * nbx * desc->bpb,
                    rows.data() + r * row_bytes + static_cast<u64>(bx0) * desc->bpb,
                    static_cast<std::size_t>(nbx) * desc->bpb);
    std::vector<u8> rgba;
    if (!DecodeBlocks(*desc, grid.data(), grid.size(), nbx, nby, astc, rgba))
        return std::nullopt;
    const u32 gw = nbx * desc->bw;
    const i64 ox = static_cast<i64>(bx0) * desc->bw, oy = static_cast<i64>(by0) * desc->bh;
    for (i64 y = cy0; y < cy1; ++y)
        std::memcpy(out.rgba.data() + (static_cast<std::size_t>(y - y0) * out.width + (cx0 - x0)) * 4,
                    rgba.data() + (static_cast<std::size_t>(y - oy) * gw + (cx0 - ox)) * 4,
                    static_cast<std::size_t>(cx1 - cx0) * 4);
    return out;
}

void FlipVertical(Image& img) {
    const std::size_t row = static_cast<std::size_t>(img.width) * 4;
    std::vector<u8> tmp(row);
    for (u32 y = 0; y < img.height / 2; ++y) {
        u8* a = img.rgba.data() + y * row;
        u8* b = img.rgba.data() + (img.height - 1 - y) * row;
        std::memcpy(tmp.data(), a, row);
        std::memcpy(a, b, row);
        std::memcpy(b, tmp.data(), row);
    }
}

/// PIL Image.transpose for the sprite packing rotations.
Image Transform(const Image& in, u32 rotation) {
    Image out;
    const u32 W = in.width, H = in.height;
    const bool swap = rotation == 4;
    out.width = swap ? H : W;
    out.height = swap ? W : H;
    out.rgba.resize(in.rgba.size());
    for (u32 y = 0; y < out.height; ++y)
        for (u32 x = 0; x < out.width; ++x) {
            u32 sx = x, sy = y;
            switch (rotation) {
            case 1: // FLIP_LEFT_RIGHT
                sx = W - 1 - x;
                break;
            case 2: // FLIP_TOP_BOTTOM
                sy = H - 1 - y;
                break;
            case 3: // ROTATE_180
                sx = W - 1 - x;
                sy = H - 1 - y;
                break;
            case 4: // ROTATE_270 (90 degrees clockwise)
                sx = y;
                sy = H - 1 - x;
                break;
            default:
                break;
            }
            std::memcpy(out.rgba.data() + (static_cast<std::size_t>(y) * out.width + x) * 4,
                        in.rgba.data() + (static_cast<std::size_t>(sy) * W + sx) * 4, 4);
        }
    return out;
}

// ---- tight sprites: UnityPy's mask_sprite with Pillow 12.3's polygon fill --------------------

struct SpriteMesh {
    std::vector<std::array<double, 3>> verts;
    std::vector<std::array<u32, 3>> tris;
};

double HalfToDouble(u16 h) {
    const int e = (h >> 10) & 0x1F, m = h & 0x3FF;
    const double s = (h & 0x8000) ? -1.0 : 1.0;
    if (e == 0)
        return s * std::ldexp(m, -24);
    if (e == 31)
        return m ? std::nan("") : s * INFINITY;
    return s * std::ldexp(1024 + m, e - 25);
}

/// m_RD vertex positions (channel 0) and triangle list, as UnityPy's MeshHandler (2019+).
std::optional<SpriteMesh> ParseSpriteMesh(const Value& rd) {
    const Value* vd = rd.Get("m_VertexData");
    const Value* channels = vd ? vd->Get("m_Channels") : nullptr;
    const Value* data = vd ? vd->Get("m_DataSize") : nullptr;
    const Value* count_v = vd ? vd->Get("m_VertexCount") : nullptr;
    const Value* index = rd.Get("m_IndexBuffer");
    const Value* subs = rd.Get("m_SubMeshes");
    if (!channels || !data || !count_v || !index || !subs || channels->items.empty())
        return std::nullopt;
    const u64 count = static_cast<u64>(count_v->AsInt());
    if (count == 0 || count > 1u << 20)
        return std::nullopt;
    struct Ch {
        u32 stream, offset, format, dim;
    };
    std::vector<Ch> ch;
    for (const Value& c : channels->items) {
        const auto g = [&](std::string_view k) {
            const Value* x = c.Get(k);
            return x ? static_cast<u32>(x->AsInt()) : 0u;
        };
        ch.push_back({g("stream"), g("offset"), g("format"), g("dimension")});
        if (ch.back().stream >= 8) // also keeps stream + 1 below from wrapping (-1 in the file)
            return std::nullopt;
    }
    const auto comp_size = [](u32 format) -> u32 {
        switch (format) {
        case 0:
        case 10:
        case 11:
            return 4;
        case 1:
        case 4:
        case 5:
        case 8:
        case 9:
            return 2;
        default:
            return 1;
        }
    };
    // get_streams: per stream, stride = sum of its channels; streams are 16-aligned.
    u32 nstreams = 0;
    for (const Ch& c : ch)
        nstreams = std::max(nstreams, c.stream + 1);
    if (nstreams > 8)
        return std::nullopt;
    std::vector<u64> s_off(nstreams), s_stride(nstreams);
    u64 off = 0;
    for (u32 s = 0; s < nstreams; ++s) {
        u64 stride = 0;
        for (const Ch& c : ch)
            if (c.stream == s && c.dim > 0)
                stride += (c.dim & 0xF) * comp_size(c.format);
        s_off[s] = off;
        s_stride[s] = stride;
        off += count * stride;
        off = (off + 15) & ~u64{15};
    }
    const Ch& pos = ch[0];
    if (pos.stream >= nstreams)
        return std::nullopt;
    if ((pos.dim & 0xF) < 2 || (pos.format != 0 && pos.format != 1))
        return std::nullopt;
    const std::string& raw = data->s;
    const u32 csz = comp_size(pos.format), dim = pos.dim & 0xF;
    SpriteMesh mesh;
    mesh.verts.resize(count);
    for (u64 v = 0; v < count; ++v) {
        const u64 at = s_off[pos.stream] + v * s_stride[pos.stream] + pos.offset;
        if (at + static_cast<u64>(dim) * csz > raw.size())
            return std::nullopt;
        for (u32 k = 0; k < 3; ++k) {
            double x = 0.0;
            if (k < dim) {
                const auto* p = reinterpret_cast<const u8*>(raw.data()) + at + k * csz;
                if (pos.format == 0) {
                    const u32 bits = u32{p[0]} | u32{p[1]} << 8 | u32{p[2]} << 16 | u32{p[3]} << 24;
                    float f;
                    std::memcpy(&f, &bits, 4);
                    x = f;
                } else {
                    x = HalfToDouble(static_cast<u16>(p[0] | p[1] << 8));
                }
            }
            mesh.verts[v][k] = x;
        }
    }
    const std::string& ib = index->s;
    const std::size_t nindex = ib.size() / 2;
    const auto idx = [&](std::size_t i) {
        return static_cast<u32>(static_cast<u8>(ib[i * 2]) | static_cast<u8>(ib[i * 2 + 1]) << 8);
    };
    for (const Value& sm : subs->items) {
        const Value* fb = sm.Get("firstByte");
        const Value* ic = sm.Get("indexCount");
        const Value* topo = sm.Get("topology");
        if (!fb || !ic || (topo && topo->AsInt() != 0))
            return std::nullopt; // only triangle lists (what Unity 2019 writes for sprites)
        const std::size_t first = static_cast<std::size_t>(fb->AsInt()) / 2;
        const std::size_t n = static_cast<std::size_t>(ic->AsInt());
        for (std::size_t i = first; i + 3 <= first + n; i += 3) {
            if (i + 3 > nindex)
                return std::nullopt;
            const std::array<u32, 3> t{idx(i), idx(i + 1), idx(i + 2)};
            if (t[0] >= count || t[1] >= count || t[2] >= count)
                return std::nullopt;
            mesh.tris.push_back(t);
        }
    }
    if (mesh.tris.empty())
        return std::nullopt;
    return mesh;
}

int PilRoundUp(float f) { // Draw.c ROUND_UP
    return static_cast<int>(f >= 0.0 ? std::floor(f + 0.5f) : -std::floor(std::fabs(static_cast<double>(f)) + 0.5f));
}
int PilRoundDown(float f) { // Draw.c ROUND_DOWN
    return static_cast<int>(f >= 0.0 ? std::ceil(f - 0.5f) : -std::ceil(std::fabs(static_cast<double>(f)) - 0.5f));
}

struct PilEdge {
    int x0, y0, xmin, ymin, xmax, ymax, d;
    float dx;
};

PilEdge PilAddEdge(int x0, int y0, int x1, int y1) {
    PilEdge e{};
    e.xmin = std::min(x0, x1);
    e.xmax = std::max(x0, x1);
    e.ymin = std::min(y0, y1);
    e.ymax = std::max(y0, y1);
    if (y0 == y1) {
        e.d = 0;
        e.dx = 0.0f;
    } else {
        e.dx = static_cast<float>(x1 - x0) / static_cast<float>(y1 - y0);
        e.d = y0 == e.ymin ? 1 : -1;
    }
    e.x0 = x0;
    e.y0 = y0;
    return e;
}

/// ImagingDrawPolygon(fill=1) on an 8-bit w x h mask (ink 1): Pillow 12.3 Draw.c, verbatim logic.
void PilFillPolygon(std::vector<u8>& im, int w, int h, const int* xy, int count) {
    const auto hline = [&](int x0, int y0, int x1) {
        if (y0 < 0 || y0 >= h)
            return;
        if (x0 < 0)
            x0 = 0;
        else if (x0 >= w)
            return;
        if (x1 < 0)
            return;
        if (x1 >= w)
            x1 = w - 1;
        for (int x = x0; x <= x1; ++x)
            im[static_cast<std::size_t>(y0) * w + x] = 1;
    };
    std::vector<PilEdge> e;
    int i;
    for (i = 0; i < count - 1; ++i) {
        const int x0 = xy[i * 2], y0 = xy[i * 2 + 1], x1 = xy[i * 2 + 2], y1 = xy[i * 2 + 3];
        if (y0 == y1 && i != 0 && y0 == xy[i * 2 - 1]) {
            PilEdge& last = e.back();
            if (x1 > x0 && x0 > xy[i * 2 - 2]) {
                last.xmax = x1;
                continue;
            } else if (x1 < x0 && x0 < xy[i * 2 - 2]) {
                last.xmin = x1;
                continue;
            }
        }
        e.push_back(PilAddEdge(x0, y0, x1, y1));
    }
    if (xy[i * 2] != xy[0] || xy[i * 2 + 1] != xy[1])
        e.push_back(PilAddEdge(xy[i * 2], xy[i * 2 + 1], xy[0], xy[1]));
    // polygon_generic, hasAlpha = 0
    const int n = static_cast<int>(e.size());
    if (n <= 0)
        return;
    std::vector<PilEdge*> table;
    int ymin = h - 1, ymax = 0;
    for (auto& ed : e) {
        ymin = std::min(ymin, ed.ymin);
        ymax = std::max(ymax, ed.ymax);
        if (ed.ymin == ed.ymax) {
            hline(ed.xmin, ed.ymin, ed.xmax);
            continue;
        }
        table.push_back(&ed);
    }
    if (ymin < 0)
        ymin = 0;
    if (ymax > h)
        ymax = h;
    const int edge_count = static_cast<int>(table.size());
    std::vector<float> xx(static_cast<std::size_t>(edge_count) * 2 + 2);
    for (; ymin <= ymax; ymin++) {
        int j = 0;
        for (int a = 0; a < edge_count; a++) {
            const PilEdge* cur = table[static_cast<std::size_t>(a)];
            if (ymin >= cur->ymin && ymin <= cur->ymax) {
                xx[static_cast<std::size_t>(j++)] = static_cast<float>(ymin - cur->y0) * cur->dx + static_cast<float>(cur->x0);
                if (ymin == cur->ymax && ymin < ymax) {
                    xx[static_cast<std::size_t>(j)] = xx[static_cast<std::size_t>(j - 1)];
                    j++;
                } else if ((ymin == cur->ymin || ymin == cur->ymax) && cur->dx != 0) {
                    for (int k = 0; k < a; k++) {
                        const PilEdge* other = table[static_cast<std::size_t>(k)];
                        if ((ymin != other->ymin && ymin != other->ymax) || other->dx == 0)
                            continue;
                        if (std::roundf(xx[static_cast<std::size_t>(j - 1)]) ==
                            std::roundf(static_cast<float>(ymin - other->y0) * other->dx + static_cast<float>(other->x0))) {
                            const int offset = ymin == cur->ymax ? -1 : 1;
                            const float adj = static_cast<float>(ymin + offset - cur->y0) * cur->dx + static_cast<float>(cur->x0);
                            if (ymin + offset >= other->ymin && ymin + offset <= other->ymax) {
                                const float adj_other = static_cast<float>(ymin + offset - other->y0) * other->dx +
                                                        static_cast<float>(other->x0);
                                float& x = xx[static_cast<std::size_t>(j - 1)];
                                if (x > adj + 1 && x > adj_other + 1) {
                                    x = std::roundf(static_cast<float>(std::fmax(adj, adj_other))) + 1;
                                } else if (x < adj - 1 && x < adj_other - 1) {
                                    x = std::roundf(static_cast<float>(std::fmin(adj, adj_other))) - 1;
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
        std::sort(xx.begin(), xx.begin() + j);
        for (int a = 1; a < j; a += 2)
            hline(PilRoundUp(xx[static_cast<std::size_t>(a - 1)]), ymin, PilRoundDown(xx[static_cast<std::size_t>(a)]));
    }
}

/// UnityPy SpriteHelper.mask_sprite on the raw-orientation crop (before the final flip).
void MaskSprite(Image& img, const SpriteMesh& mesh, double pixels_to_units) {
    double min_x = mesh.verts[0][0], min_y = mesh.verts[0][1];
    for (const auto& v : mesh.verts) {
        min_x = std::min(min_x, v[0]);
        min_y = std::min(min_y, v[1]);
    }
    const int w = static_cast<int>(img.width), h = static_cast<int>(img.height);
    std::vector<u8> mask(static_cast<std::size_t>(w) * h, 0);
    for (const auto& t : mesh.tris) {
        int xy[6];
        for (int k = 0; k < 3; ++k) {
            const auto& v = mesh.verts[t[static_cast<std::size_t>(k)]];
            // C (int) cast; a corrupt mesh is kept within +-2^20 (no overflow in the polygon fill)
            const auto px = [](double d) {
                return std::clamp(SaturatingCast<int>(d), -(1 << 20), 1 << 20);
            };
            xy[k * 2] = px((v[0] - min_x) * pixels_to_units);
            xy[k * 2 + 1] = px((v[1] - min_y) * pixels_to_units);
        }
        PilFillPolygon(mask, w, h, xy, 3);
    }
    for (std::size_t p = 0; p < mask.size(); ++p)
        if (!mask[p])
            std::memset(img.rgba.data() + p * 4, 0, 4);
}

std::optional<TextureInfo> TextureOf(const Value& v) {
    TextureInfo t;
    if (const Value* n = v.Get("m_Name"))
        t.name = std::string{n->AsString()};
    const auto geti = [&](std::string_view k) {
        const Value* x = v.Get(k);
        return x ? static_cast<i32>(x->AsInt()) : 0;
    };
    t.width = geti("m_Width");
    t.height = geti("m_Height");
    t.format = geti("m_TextureFormat");
    if (const Value* blob = v.Get("m_PlatformBlob")) {
        if (blob->kind == Value::Kind::Bytes)
            t.platform_blob.assign(blob->s.begin(), blob->s.end());
        else
            for (const Value& b : blob->items)
                t.platform_blob.push_back(static_cast<u8>(b.AsInt()));
    }
    if (const Value* d = v.Get("image data"))
        t.image_data.assign(d->s.begin(), d->s.end());
    if (const Value* s = v.Get("m_StreamData")) {
        if (const Value* x = s->Get("offset"))
            t.stream_offset = static_cast<u64>(x->AsInt());
        if (const Value* x = s->Get("size"))
            t.stream_size = static_cast<u32>(x->AsInt());
        if (const Value* x = s->Get("path"))
            t.stream_path = std::string{x->AsString()};
    }
    return t;
}

} // namespace

std::vector<std::uint8_t> SwitchDeswizzle(std::span<const std::uint8_t> data, std::uint32_t width,
                                          std::uint32_t height, std::uint32_t block_width,
                                          std::uint32_t block_height,
                                          std::uint32_t gobs_per_block) {
    try {
        if (block_width == 0 || block_height == 0 || gobs_per_block == 0)
            return {};
        const u32 bcx = CeilDiv(width, block_width), bcy = CeilDiv(height, block_height);
        const u32 gcx = bcx / 4, gcy = bcy / 8;
        std::vector<u8> out(data.size());
        std::size_t src = 0;
        for (u32 i = 0; i < gcy / gobs_per_block; ++i)
            for (u32 j = 0; j < gcx; ++j)
                for (u32 k = 0; k < gobs_per_block; ++k) {
                    const u32 base_y = (i * gobs_per_block + k) * 8, base_x = j * 4;
                    for (u32 v = 0; v < 32; ++v) {
                        const u32 gx = ((v >> 3) & 2) | ((v >> 1) & 1);
                        const u32 gy = ((v >> 1) & 6) | (v & 1);
                        const u64 dst = (static_cast<u64>(base_y + gy) * bcx + base_x + gx) * 16;
                        if (src + 16 > data.size() || dst + 16 > out.size())
                            return {};
                        std::memcpy(out.data() + dst, data.data() + src, 16);
                        src += 16;
                    }
                }
        return out;
    } catch (...) {
        return {};
    }
}

std::optional<Image> DecodeTextureData(std::span<const std::uint8_t> data, const TextureInfo& tex,
                                       const AstcDecoder& astc, std::int32_t platform) {
    try {
        const DataFetch fetch = [&](u64 off, u8* out, std::size_t n) {
            if (off > data.size() || n > data.size() - off)
                return false;
            std::memcpy(out, data.data() + off, n);
            return true;
        };
        auto img = DecodeRegion(fetch, data.size(), tex, platform, astc, 0, 0, tex.width, tex.height);
        if (img)
            FlipVertical(*img);
        return img;
    } catch (...) {
        return std::nullopt;
    }
}

// ---- Reader ------------------------------------------------------------------------------------

struct AtlasEntry {
    PPtr texture, alpha;
    Rectf rect;
    float off_x{}, off_y{};
    u32 settings{};
};

struct Reader::Loaded {
    std::shared_ptr<Bundle> bundle;
    std::shared_ptr<SerializedFile> file; // first serialized node (atlases, ListObjects)
    // Every serialized node in node order: scene bundles carry one CAB-x.sharedAssets per scene
    // (plus the scene CABs); asset bundles have one. Names index across all of them, first wins.
    std::vector<std::shared_ptr<SerializedFile>> files;
    std::mutex mutex;
    bool indexed{};
    struct Hit {
        u32 file{};
        i64 id{};
    };
    std::vector<std::pair<std::string, i64>> sprites;  // object order
    std::vector<std::pair<std::string, i64>> textures; // object order
    std::unordered_map<std::string, Hit> sprite_by_name;
    std::unordered_map<std::string, Hit> texture_by_name;
    // SpriteAtlas render data per atlas path id, keyed by GUID bytes + second.
    std::map<i64, std::shared_ptr<std::map<std::string, AtlasEntry>>> atlases;
    std::vector<std::pair<std::string, i64>> atlas_names;

    void Index() {
        if (indexed)
            return;
        indexed = true;
        for (u32 fi = 0; fi < files.size(); ++fi) {
            const SerializedFile& f = *files[fi];
            for (const ObjectInfo& o : f.Objects()) {
                if (o.class_id != ClassSprite && o.class_id != ClassTexture2D &&
                    (o.class_id != ClassSpriteAtlas || fi != 0))
                    continue;
                auto name = f.PeekName(o);
                Count(g_stats.objects_indexed);
                if (!name)
                    continue;
                if (o.class_id == ClassSprite) {
                    sprite_by_name.emplace(*name, Hit{fi, o.path_id});
                    sprites.emplace_back(std::move(*name), o.path_id);
                } else if (o.class_id == ClassTexture2D) {
                    texture_by_name.emplace(*name, Hit{fi, o.path_id});
                    textures.emplace_back(std::move(*name), o.path_id);
                } else {
                    atlas_names.emplace_back(std::move(*name), o.path_id);
                }
            }
        }
    }

    std::shared_ptr<std::map<std::string, AtlasEntry>> Atlas(i64 path_id) {
        if (const auto it = atlases.find(path_id); it != atlases.end())
            return it->second;
        const ObjectInfo* o = file->Find(path_id);
        if (!o || o->class_id != ClassSpriteAtlas)
            return nullptr;
        const auto v = file->ReadObject(*o);
        if (!v)
            return nullptr;
        auto map = std::make_shared<std::map<std::string, AtlasEntry>>();
        if (const Value* rdm = v->Get("m_RenderDataMap")) {
            for (const Value& pair : rdm->items) {
                const Value* key = pair.Get("first");
                const Value* data = pair.Get("second");
                if (!key || !data)
                    continue;
                u8 guid[16];
                GuidOf(key->Get("first"), guid);
                const Value* second = key->Get("second");
                const i64 sec = second ? second->AsInt() : 0;
                std::string k(reinterpret_cast<const char*>(guid), 16);
                k.append(reinterpret_cast<const char*>(&sec), 8);
                AtlasEntry e;
                e.texture = PtrOf(data->Get("texture"));
                e.alpha = PtrOf(data->Get("alphaTexture"));
                e.rect = RectOf(data->Get("textureRect"));
                if (const Value* off = data->Get("textureRectOffset")) {
                    e.off_x = F(off->Get("x"));
                    e.off_y = F(off->Get("y"));
                }
                if (const Value* s = data->Get("settingsRaw"))
                    e.settings = static_cast<u32>(s->AsInt());
                map->emplace(std::move(k), e);
            }
        }
        atlases.emplace(path_id, map);
        return map;
    }
};

struct Reader::Impl {
    std::atomic<bool> use_pins{true};
    RangeReader read;
    std::string root;
    std::size_t max_bundles{};
    std::mutex mutex;
    AstcDecoder astc;
    std::list<std::pair<std::string, std::shared_ptr<Loaded>>> lru;

    AstcDecoder Astc() {
        std::lock_guard lock{mutex};
        return astc;
    }

    std::shared_ptr<Loaded> Open(std::string_view rel) {
        const std::string path = root + std::string{rel};
        {
            std::lock_guard lock{mutex};
            for (auto it = lru.begin(); it != lru.end(); ++it)
                if (it->first == path) {
                    lru.splice(lru.begin(), lru, it);
                    return lru.front().second;
                }
        }
        if (!read)
            return nullptr;
        const auto size = read.size(path);
        if (!size)
            return nullptr;
        ByteSource src;
        src.size = *size;
        RangeReader r = read;
        src.read = [r, path](u64 off, u8* out, std::size_t n) { return r.read(path, off, out, n); };
        auto loaded = std::make_shared<Loaded>();
        loaded->bundle = Bundle::Open(std::move(src));
        if (!loaded->bundle)
            return nullptr;
        // Scene bundles: the assets (sprites, textures) live in the CAB-x.sharedAssets nodes; the
        // scene CABs beside them hold only the scenes' GameObjects and are large, so a bundle that
        // has sharedAssets nodes opens only those.
        bool scene_bundle = false;
        for (const BundleNode& n : loaded->bundle->Nodes())
            scene_bundle |= n.path.ends_with(".sharedAssets");
        for (const BundleNode& n : loaded->bundle->Nodes()) {
            if (n.path.ends_with(".resS") || n.path.ends_with(".resource") ||
                (scene_bundle && !n.path.ends_with(".sharedAssets")))
                continue;
            if (auto f = SerializedFile::Open(loaded->bundle->NodeSource(n)))
                loaded->files.push_back(std::move(f));
        }
        if (loaded->files.empty())
            return nullptr;
        loaded->file = loaded->files.front();
        std::lock_guard lock{mutex};
        for (auto it = lru.begin(); it != lru.end(); ++it)
            if (it->first == path) // another thread opened it meanwhile: share that one
                return it->second;
        lru.emplace_front(path, loaded);
        while (lru.size() > std::max<std::size_t>(max_bundles, 1))
            lru.pop_back();
        return loaded;
    }

    /// Raw-orientation region of a texture object of this bundle.
    /// `fi` is the serialized file the pointer was read from (file id 0 = that file).
    std::optional<Image> Region(Loaded& l, u32 fi, const PPtr& ptr, i64 x0, i64 y0, i64 x1, i64 y1) {
        if (ptr.file_id != 0 || fi >= l.files.size())
            return std::nullopt; // texture in another serialized file: not used by this game
        const SerializedFile& sf = *l.files[fi];
        const ObjectInfo* o = sf.Find(ptr.path_id);
        if (!o || o->class_id != ClassTexture2D)
            return std::nullopt;
        const auto v = sf.ReadObject(*o);
        if (!v)
            return std::nullopt;
        const auto tex = TextureOf(*v);
        if (!tex)
            return std::nullopt;
        DataFetch fetch;
        u64 data_size;
        if (tex->stream_size > 0 && !tex->stream_path.empty()) {
            const BundleNode* node = l.bundle->FindNode(tex->stream_path);
            if (!node || tex->stream_offset > node->size ||
                tex->stream_size > node->size - tex->stream_offset)
                return std::nullopt;
            const u64 base = node->offset + tex->stream_offset;
            data_size = tex->stream_size;
            Bundle* b = l.bundle.get();
            fetch = [b, base, data_size](u64 off, u8* out, std::size_t n) {
                if (off > data_size || n > data_size - off)
                    return false;
                return b->Read(base + off, out, n);
            };
        } else {
            const auto& d = tex->image_data;
            data_size = d.size();
            fetch = [&d](u64 off, u8* out, std::size_t n) {
                if (off > d.size() || n > d.size() - off)
                    return false;
                std::memcpy(out, d.data() + off, n);
                return true;
            };
        }
        return DecodeRegion(fetch, data_size, *tex, sf.Platform(), Astc(), x0, y0, x1, y1);
    }

    std::optional<SpriteInfo> Sprite(Loaded& l, std::string_view name,
                                     std::optional<SpriteMesh>* mesh_out = nullptr,
                                     u32* file_out = nullptr, const Loaded::Hit* hint = nullptr) {
        std::lock_guard lock{l.mutex};
        Loaded::Hit hit;
        bool hinted = false;
        if (hint && hint->file < l.files.size()) {
            // A pinned (file, path id) for this build: verified by class and name, so a different
            // game build falls back to the name index instead of decoding the wrong object.
            const SerializedFile& hf = *l.files[hint->file];
            const ObjectInfo* ho = hf.Find(hint->id);
            if (ho && ho->class_id == ClassSprite) {
                const auto n = hf.PeekName(*ho);
                hinted = n && *n == name;
            }
            if (hinted)
                hit = *hint;
        }
        if (!hinted) {
            l.Index();
            const auto it = l.sprite_by_name.find(std::string{name});
            if (it == l.sprite_by_name.end())
                return std::nullopt;
            hit = it->second;
        }
        const SerializedFile& sf = *l.files[hit.file];
        if (file_out)
            *file_out = hit.file;
        const ObjectInfo* o = sf.Find(hit.id);
        if (!o)
            return std::nullopt;
        const auto v = sf.ReadObject(*o);
        if (!v)
            return std::nullopt;
        SpriteInfo s;
        s.name = std::string{name};
        s.rect = RectOf(v->Get("m_Rect"));
        if (const Value* off = v->Get("m_Offset")) {
            s.offset_x = F(off->Get("x"));
            s.offset_y = F(off->Get("y"));
        }
        s.pixels_to_units = F(v->Get("m_PixelsToUnits"));
        if (const Value* pv = v->Get("m_Pivot")) {
            s.pivot_x = F(pv->Get("x"));
            s.pivot_y = F(pv->Get("y"));
        }
        if (const Value* tags = v->Get("m_AtlasTags"))
            for (const Value& t : tags->items)
                s.atlas_tags.emplace_back(t.AsString());
        s.sprite_atlas = PtrOf(v->Get("m_SpriteAtlas"));
        if (const Value* key = v->Get("m_RenderDataKey")) {
            GuidOf(key->Get("first"), s.render_key_guid);
            if (const Value* sec = key->Get("second"))
                s.render_key_second = sec->AsInt();
        }
        const Value* rd = v->Get("m_RD");
        if (mesh_out)
            *mesh_out = rd ? ParseSpriteMesh(*rd) : std::nullopt;
        s.texture = PtrOf(rd ? rd->Get("texture") : nullptr);
        s.alpha_texture = PtrOf(rd ? rd->Get("alphaTexture") : nullptr);
        s.texture_rect = RectOf(rd ? rd->Get("textureRect") : nullptr);
        if (const Value* off = rd ? rd->Get("textureRectOffset") : nullptr) {
            s.texture_rect_offset_x = F(off->Get("x"));
            s.texture_rect_offset_y = F(off->Get("y"));
        }
        if (const Value* sr = rd ? rd->Get("settingsRaw") : nullptr)
            s.settings_raw = static_cast<u32>(sr->AsInt());

        // Atlas lookup, as UnityPy's get_image_from_sprite.
        i64 atlas_id = 0;
        bool have_atlas = false;
        if (!s.sprite_atlas.IsNull()) {
            if (s.sprite_atlas.file_id == 0) {
                atlas_id = s.sprite_atlas.path_id;
                have_atlas = true;
            }
        } else if (!s.atlas_tags.empty()) {
            for (const auto& [aname, id] : l.atlas_names)
                if (aname == s.atlas_tags.front()) {
                    atlas_id = id;
                    have_atlas = true;
                    break;
                }
        }
        if (have_atlas && hit.file == 0) {
            if (const auto map = l.Atlas(atlas_id)) {
                std::string k(reinterpret_cast<const char*>(s.render_key_guid), 16);
                k.append(reinterpret_cast<const char*>(&s.render_key_second), 8);
                if (const auto e = map->find(k); e != map->end()) {
                    s.texture = e->second.texture;
                    s.texture_rect = e->second.rect;
                    s.texture_rect_offset_x = e->second.off_x;
                    s.texture_rect_offset_y = e->second.off_y;
                    s.settings_raw = e->second.settings;
                    s.alpha_texture = e->second.alpha;
                }
            }
        }
        return s;
    }
};

Reader::Reader(RangeReader read, AstcDecoder astc, std::string asset_root, std::size_t max_bundles)
    : impl(std::make_unique<Impl>()) {
    impl->read = std::move(read);
    impl->astc = std::move(astc);
    impl->root = std::move(asset_root);
    impl->max_bundles = max_bundles;
}

Reader::~Reader() = default;

void Reader::SetAstcDecoder(AstcDecoder astc) {
    std::lock_guard lock{impl->mutex};
    impl->astc = std::move(astc);
}

void Reader::ClearCache() {
    std::lock_guard lock{impl->mutex};
    impl->lru.clear();
}

std::vector<std::pair<std::string, std::int64_t>> Reader::ListObjects(std::string_view bundle,
                                                                      std::int32_t class_id) {
    try {
        const auto l = impl->Open(bundle);
        std::vector<std::pair<std::string, i64>> out;
        if (!l)
            return out;
        for (const ObjectInfo& o : l->file->Objects()) {
            if (class_id != -1 && o.class_id != class_id)
                continue;
            auto name = l->file->PeekName(o);
            out.emplace_back(name ? std::move(*name) : std::string{}, o.path_id);
        }
        return out;
    } catch (...) {
        return {};
    }
}

std::optional<Value> Reader::ReadObjectByName(std::string_view bundle, std::string_view name,
                                              std::int32_t class_id) {
    try {
        const auto l = impl->Open(bundle);
        if (!l)
            return std::nullopt;
        for (const ObjectInfo& o : l->file->Objects()) {
            if (class_id != -1 && o.class_id != class_id)
                continue;
            const auto n = l->file->PeekName(o);
            if (n && *n == name)
                return l->file->ReadObject(o);
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Value> Reader::ReadObject(std::string_view bundle, std::int64_t path_id) {
    try {
        const auto l = impl->Open(bundle);
        if (!l)
            return std::nullopt;
        const ObjectInfo* o = l->file->Find(path_id);
        return o ? l->file->ReadObject(*o) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}


std::vector<std::string> Reader::ListSprites(std::string_view bundle) {
    try {
        const auto l = impl->Open(bundle);
        if (!l)
            return {};
        std::lock_guard lock{l->mutex};
        l->Index();
        std::vector<std::string> out;
        for (const auto& [n, id] : l->sprites)
            out.push_back(n);
        return out;
    } catch (...) {
        return {};
    }
}

std::vector<std::string> Reader::ListTextures(std::string_view bundle) {
    try {
        const auto l = impl->Open(bundle);
        if (!l)
            return {};
        std::lock_guard lock{l->mutex};
        l->Index();
        std::vector<std::string> out;
        for (const auto& [n, id] : l->textures)
            out.push_back(n);
        return out;
    } catch (...) {
        return {};
    }
}

std::optional<SpriteInfo> Reader::GetSprite(std::string_view bundle, std::string_view sprite) {
    try {
        const auto l = impl->Open(bundle);
        if (!l)
            return std::nullopt;
        return impl->Sprite(*l, sprite);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Image> Reader::LoadTexture(std::string_view bundle, std::string_view texture) {
    try {
        const auto l = impl->Open(bundle);
        if (!l)
            return std::nullopt;
        Loaded::Hit hit;
        {
            std::lock_guard lock{l->mutex};
            l->Index();
            const auto it = l->texture_by_name.find(std::string{texture});
            if (it == l->texture_by_name.end())
                return std::nullopt;
            hit = it->second;
        }
        const ObjectInfo* o = l->files[hit.file]->Find(hit.id);
        if (!o)
            return std::nullopt;
        const auto v = l->files[hit.file]->ReadObject(*o);
        const auto tex = v ? TextureOf(*v) : std::nullopt;
        if (!tex)
            return std::nullopt;
        auto img = impl->Region(*l, hit.file, PPtr{0, hit.id}, 0, 0, tex->width, tex->height);
        if (img)
            FlipVertical(*img);
        return img;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Image> Reader::LoadSprite(std::string_view bundle, std::string_view sprite) {
    if (impl->use_pins.load(std::memory_order_relaxed))
        if (const auto pin = BuiltinPin(bundle, sprite))
            return LoadSpritePinned(bundle, sprite, *pin);
    return LoadSpritePinned(bundle, sprite, {});
}

std::optional<Image> Reader::LoadSpritePinned(std::string_view bundle, std::string_view sprite,
                                              SpritePin pin) {
    try {
        const auto l = impl->Open(bundle);
        if (!l)
            return std::nullopt;
        std::optional<SpriteMesh> mesh;
        u32 fi = 0;
        const Loaded::Hit hint{pin.file, pin.path_id};
        const auto s = impl->Sprite(*l, sprite, &mesh, &fi, pin.path_id ? &hint : nullptr);
        if (!s)
            return std::nullopt;
        const PPtr alpha = s->alpha_texture;
        // PIL crop box with Python round() on the float rect (x + width summed in double).
        const double rx = s->texture_rect.x, ry = s->texture_rect.y;
        const double rw = s->texture_rect.width, rh = s->texture_rect.height;
        constexpr double MaxCoord = 1 << 24; // far beyond any texture (Region checks the bounds)
        if (!(std::fabs(rx) < MaxCoord && std::fabs(ry) < MaxCoord && std::fabs(rw) < MaxCoord &&
              std::fabs(rh) < MaxCoord))
            return std::nullopt; // NaN or absurd rect: no float -> integer overflow
        const i64 x0 = static_cast<i64>(PyRound(rx));
        const i64 y0 = static_cast<i64>(PyRound(ry));
        const i64 x1 = static_cast<i64>(PyRound(rx + static_cast<double>(s->texture_rect.width)));
        const i64 y1 = static_cast<i64>(PyRound(ry + static_cast<double>(s->texture_rect.height)));
        auto img = impl->Region(*l, fi, s->texture, x0, y0, x1, y1);
        if (!img)
            return std::nullopt;
        if (!alpha.IsNull()) {
            const auto a = impl->Region(*l, fi, alpha, x0, y0, x1, y1);
            if (!a)
                return std::nullopt;
            for (std::size_t p = 0; p + 3 < img->rgba.size(); p += 4)
                img->rgba[p + 3] = a->rgba[p]; // alpha texture's red channel, like UnityPy
        }
        if (s->packed() && s->rotation() >= 1 && s->rotation() <= 4)
            *img = Transform(*img, s->rotation());
        // Tight packing: keep only the sprite mesh's polygons (UnityPy mask_sprite).
        if (s->tight() && mesh)
            MaskSprite(*img, *mesh, static_cast<double>(s->pixels_to_units));
        FlipVertical(*img);
        return img;
    } catch (...) {
        return std::nullopt;
    }
}


std::shared_ptr<const Bundle> Reader::OpenBundle(std::string_view bundle) {
    try {
        const auto l = impl->Open(bundle);
        return l ? std::shared_ptr<const Bundle>(l->bundle) : nullptr;
    } catch (...) {
        return nullptr;
    }
}

std::shared_ptr<const SerializedFile> Reader::OpenSerialized(std::string_view bundle) {
    try {
        const auto l = impl->Open(bundle);
        return l ? std::shared_ptr<const SerializedFile>(l->file) : nullptr;
    } catch (...) {
        return nullptr;
    }
}

void Reader::Evict(std::string_view bundle) {
    const std::string path = impl->root + std::string{bundle};
    std::lock_guard lock{impl->mutex};
    impl->lru.remove_if([&](const auto& e) { return e.first == path; });
}

std::optional<SpritePin> Reader::FindSpritePin(std::string_view bundle, std::string_view sprite) {
    try {
        const auto l = impl->Open(bundle);
        if (!l)
            return std::nullopt;
        std::lock_guard lock{l->mutex};
        l->Index();
        const auto it = l->sprite_by_name.find(std::string{sprite});
        if (it == l->sprite_by_name.end())
            return std::nullopt;
        return SpritePin{it->second.file, it->second.id};
    } catch (...) {
        return std::nullopt;
    }
}

bool Reader::HasSprite(std::string_view bundle, std::string_view sprite) {
    try {
        const auto l = impl->Open(bundle);
        if (!l)
            return false;
        std::lock_guard lock{l->mutex};
        l->Index();
        return l->sprite_by_name.contains(std::string{sprite});
    } catch (...) {
        return false;
    }
}

std::optional<Image> Reader::LoadSpriteAny(std::span<const std::string_view> bundles,
                                           std::string_view sprite, std::string* found_in) {
    if (impl->use_pins.load(std::memory_order_relaxed))
        for (const std::string_view b : bundles)
            if (const auto pin = BuiltinPin(b, sprite)) {
                if (auto img = LoadSpritePinned(b, sprite, *pin)) {
                    if (found_in)
                        *found_in = std::string{b};
                    return img;
                }
            }
    for (const std::string_view b : bundles) {
        if (!HasSprite(b, sprite))
            continue;
        if (found_in)
            *found_in = std::string{b};
        return LoadSprite(b, sprite);
    }
    return std::nullopt;
}

// ---- Chained Echoes: built-in pins ---------------------------------------------------------

namespace {
struct PinRow {
    std::string_view bundle, sprite;
    u32 file;
    i64 path_id;
};
constexpr PinRow PinRows[] = {
#include "chained_echoes_unity_pins.inc"
};
} // namespace

std::optional<SpritePin> BuiltinPin(std::string_view bundle, std::string_view sprite) {
    static const std::vector<const PinRow*> sorted = [] {
        std::vector<const PinRow*> v;
        for (const PinRow& r : PinRows)
            v.push_back(&r);
        std::sort(v.begin(), v.end(), [](const PinRow* a, const PinRow* b) {
            return std::tie(a->bundle, a->sprite) < std::tie(b->bundle, b->sprite);
        });
        return v;
    }();
    const auto it = std::lower_bound(sorted.begin(), sorted.end(), std::pair{bundle, sprite},
                                     [](const PinRow* r, const std::pair<std::string_view, std::string_view>& k) {
                                         return std::tie(r->bundle, r->sprite) < std::tie(k.first, k.second);
                                     });
    if (it == sorted.end() || (*it)->bundle != bundle || (*it)->sprite != sprite)
        return std::nullopt;
    return SpritePin{(*it)->file, (*it)->path_id};
}

std::size_t BuiltinPinCount() {
    return std::size(PinRows);
}

void Reader::SetUseBuiltinPins(bool use) {
    impl->use_pins.store(use, std::memory_order_relaxed);
}

// ---- Chained Echoes: families --------------------------------------------------------------

namespace {
constexpr std::string_view CtbBundles[] = {bundles::Gfx2,     bundles::SystemGfx,
                                           bundles::DuplicateIsolation6, bundles::Enemies2,
                                           bundles::Gfx,      bundles::BrTunnel,
                                           bundles::FiorWoods, bundles::ElrantLevels};
constexpr std::string_view BestiaryBundles[] = {bundles::Bestiary, bundles::ElrantGfx,
                                                bundles::SystemGfx};
constexpr std::string_view PortraitBundles[] = {bundles::Gfx2, bundles::PackedAssets,
                                                bundles::SystemGfx};
} // namespace

std::span<const std::string_view> FamilyBundles(Family family) {
    switch (family) {
    case Family::Ctb:
        return CtbBundles;
    case Family::Bestiary:
        return BestiaryBundles;
    case Family::Portrait:
        return PortraitBundles;
    }
    return {};
}

std::string FamilySprite(Family family, std::string_view id) {
    switch (family) {
    case Family::Ctb:
        return "ctb_" + std::string{id};
    case Family::Bestiary:
        return "bestiary_" + std::string{id};
    case Family::Portrait:
        return "portrait_" + std::string{id};
    }
    return {};
}

// ---- host / directory readers ----------------------------------------------------------------

RangeReader MakeHostRangeReader(const EdenDsmodHostApi* host) {
    RangeReader r;
    if (!host || !host->read_romfs)
        return r;
    r.size = [host](std::string_view p) -> std::optional<u64> {
        const std::string path{p};
        const std::size_t n = host->read_romfs(host->userdata, path.c_str(), 0, nullptr, 0);
        if (n == 0)
            return std::nullopt;
        return static_cast<u64>(n);
    };
    r.read = [host](std::string_view p, u64 off, u8* out, std::size_t n) {
        const std::string path{p};
        constexpr std::size_t Chunk = 32u << 20;
        while (n > 0) {
            const std::size_t k = std::min(n, Chunk);
            if (host->read_romfs(host->userdata, path.c_str(), off, out, k) != k)
                return false;
            off += k;
            out += k;
            n -= k;
        }
        return true;
    };
    return r;
}

RangeReader MakeDirectoryRangeReader(std::string root) {
    if (!root.empty() && root.back() != '/')
        root += '/';
    RangeReader r;
    const auto full = [root](std::string_view p) {
        while (!p.empty() && p.front() == '/')
            p.remove_prefix(1);
        return root + std::string{p};
    };
    r.size = [full](std::string_view p) -> std::optional<u64> {
        std::FILE* f = std::fopen(full(p).c_str(), "rb");
        if (!f)
            return std::nullopt;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fclose(f);
        if (n < 0)
            return std::nullopt;
        return static_cast<u64>(n);
    };
    r.read = [full](std::string_view p, u64 off, u8* out, std::size_t n) {
        std::FILE* f = std::fopen(full(p).c_str(), "rb");
        if (!f)
            return false;
        const bool ok = std::fseek(f, static_cast<long>(off), SEEK_SET) == 0 &&
                        std::fread(out, 1, n, f) == n;
        std::fclose(f);
        return ok;
    };
    return r;
}

// ---- loose serialized files ---------------------------------------------------------------------

std::optional<std::string> ReadTextAsset(const RangeReader& read, std::string_view assets_path,
                                         std::string_view name, std::int64_t path_id) {
    try {
        if (!read)
            return std::nullopt;
        const std::string path{assets_path};
        const auto size = read.size(path);
        if (!size)
            return std::nullopt;
        ByteSource src;
        src.size = *size;
        src.read = [read, path](u64 off, u8* out, std::size_t n) { return read.read(path, off, out, n); };
        const auto sf = SerializedFile::Open(std::move(src));
        if (!sf)
            return std::nullopt;
        constexpr i32 ClassTextAsset = 49;
        for (const ObjectInfo& o : sf->Objects()) {
            if (o.class_id != ClassTextAsset || (name.empty() && o.path_id != path_id))
                continue;
            if (!name.empty()) {
                const auto n = sf->PeekName(o);
                if (!n || *n != name)
                    continue;
            }
            if (sf->HasTypeTrees()) {
                const auto v = sf->ReadObject(o);
                const Value* script = v ? v->Get("m_Script") : nullptr;
                return script ? std::optional<std::string>{std::string{script->AsString()}} : std::nullopt;
            }
            const auto raw = sf->ReadRaw(o);
            if (!raw)
                return std::nullopt;
            Cursor c{raw->data(), raw->size(), 0, 0, false};
            std::string skip, script;
            const i32 nlen = c.Int<i32>();
            if (nlen < 0 || !c.Bytes(skip, static_cast<std::size_t>(nlen)))
                return std::nullopt;
            c.Align();
            const i32 slen = c.Int<i32>();
            if (slen < 0 || !c.Bytes(script, static_cast<std::size_t>(slen)))
                return std::nullopt;
            return script;
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

// ---- TextMesh Pro font -------------------------------------------------------------------------

const TmpGlyph* TmpFont::Find(std::uint32_t codepoint) const {
    const auto it = chars.find(codepoint);
    return it == chars.end() || it->second >= glyphs.size() ? nullptr : &glyphs[it->second];
}

std::optional<TmpFont> LoadTmpFont(Reader& reader, std::string_view bundle, std::string_view name,
                                   bool with_atlas) {
    try {
        const auto v = reader.ReadObjectByName(bundle, name, 114);
        if (!v || !v->Get("m_GlyphTable") || !v->Get("m_CharacterTable"))
            return std::nullopt;
        TmpFont f;
        f.name = std::string{name};
        if (const Value* fi = v->Get("m_FaceInfo")) {
            const auto g = [&](std::string_view k) { return F(fi->Get(k)); };
            if (const Value* x = fi->Get("m_FamilyName"))
                f.family = std::string{x->AsString()};
            if (const Value* x = fi->Get("m_StyleName"))
                f.style = std::string{x->AsString()};
            f.point_size = g("m_PointSize");
            f.scale = g("m_Scale");
            f.line_height = g("m_LineHeight");
            f.ascent = g("m_AscentLine");
            f.cap_line = g("m_CapLine");
            f.mean_line = g("m_MeanLine");
            f.baseline = g("m_Baseline");
            f.descent = g("m_DescentLine");
        }
        const auto geti = [&](std::string_view k) {
            const Value* x = v->Get(k);
            return x ? static_cast<i32>(x->AsInt()) : 0;
        };
        f.atlas_w = geti("m_AtlasWidth");
        f.atlas_h = geti("m_AtlasHeight");
        f.padding = geti("m_AtlasPadding");
        std::unordered_map<u32, u32> by_index;
        for (const Value& g : v->Get("m_GlyphTable")->items) {
            TmpGlyph t;
            if (const Value* x = g.Get("m_Index"))
                t.index = static_cast<u32>(x->AsInt());
            if (const Value* m = g.Get("m_Metrics")) {
                t.width = F(m->Get("m_Width"));
                t.height = F(m->Get("m_Height"));
                t.bearing_x = F(m->Get("m_HorizontalBearingX"));
                t.bearing_y = F(m->Get("m_HorizontalBearingY"));
                t.advance = F(m->Get("m_HorizontalAdvance"));
            }
            if (const Value* r = g.Get("m_GlyphRect")) {
                const auto ri = [&](std::string_view k) {
                    const Value* x = r->Get(k);
                    return x ? static_cast<i32>(x->AsInt()) : 0;
                };
                t.rect_x = ri("m_X");
                t.rect_y = ri("m_Y");
                t.rect_w = ri("m_Width");
                t.rect_h = ri("m_Height");
            }
            if (const Value* x = g.Get("m_Scale"))
                t.scale = static_cast<float>(x->AsFloat(1.0));
            if (const Value* x = g.Get("m_AtlasIndex"))
                t.atlas_index = static_cast<i32>(x->AsInt());
            by_index.emplace(t.index, static_cast<u32>(f.glyphs.size()));
            f.glyphs.push_back(t);
        }
        for (const Value& c : v->Get("m_CharacterTable")->items) {
            const Value* u = c.Get("m_Unicode");
            const Value* gi = c.Get("m_GlyphIndex");
            if (!u || !gi)
                continue;
            const auto it = by_index.find(static_cast<u32>(gi->AsInt()));
            if (it != by_index.end())
                f.chars.emplace(static_cast<u32>(u->AsInt()), it->second);
        }
        if (const Value* at = v->Get("m_AtlasTextures"); at && !at->items.empty())
            f.atlas = PtrOf(&at->items[0]);
        if (with_atlas) {
            if (f.atlas.file_id != 0 || f.atlas.IsNull())
                return std::nullopt;
            const auto tv = reader.ReadObject(bundle, f.atlas.path_id);
            const Value* tn = tv ? tv->Get("m_Name") : nullptr;
            if (!tn)
                return std::nullopt;
            auto img = reader.LoadTexture(bundle, tn->AsString());
            if (!img)
                return std::nullopt;
            f.atlas_image = std::move(*img);
        }
        return f;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Image> GlyphTile(const TmpFont& font, std::uint32_t codepoint, int threshold) {
    const TmpGlyph* g = font.Find(codepoint);
    const Image& a = font.atlas_image;
    if (!g || a.rgba.empty() || g->rect_w <= 0 || g->rect_h <= 0)
        return std::nullopt;
    const i64 x0 = g->rect_x, y0 = static_cast<i64>(a.height) - g->rect_y - g->rect_h;
    if (x0 < 0 || y0 < 0 || x0 + g->rect_w > a.width || y0 + g->rect_h > a.height)
        return std::nullopt;
    Image t;
    t.width = static_cast<u32>(g->rect_w);
    t.height = static_cast<u32>(g->rect_h);
    t.rgba.resize(static_cast<std::size_t>(t.width) * t.height * 4);
    for (u32 y = 0; y < t.height; ++y)
        for (u32 x = 0; x < t.width; ++x) {
            const u8 av = a.rgba[((static_cast<std::size_t>(y0 + y) * a.width) + x0 + x) * 4 + 3];
            u8* q = t.rgba.data() + (static_cast<std::size_t>(y) * t.width + x) * 4;
            q[0] = q[1] = q[2] = 0xFF;
            q[3] = threshold < 0 ? av : (av >= threshold ? 0xFF : 0);
        }
    return t;
}

Image HostFontAtlas(const TmpFont& font, int threshold) {
    Image out = font.atlas_image;
    for (std::size_t p = 0; p + 3 < out.rgba.size(); p += 4) {
        out.rgba[p] = out.rgba[p + 1] = out.rgba[p + 2] = 0xFF;
        if (threshold >= 0)
            out.rgba[p + 3] = out.rgba[p + 3] >= threshold ? 0xFF : 0;
    }
    return out;
}

bool BuildHostGlyphs(const TmpFont& font, std::uint32_t& first_codepoint,
                     std::vector<EdenDsmodFontGlyph>& glyphs, std::uint32_t& line_height,
                     std::uint32_t max_codepoint) {
    glyphs.clear();
    u32 lo = 0, hi = 0;
    bool any = false;
    for (const auto& [cp, gi] : font.chars) {
        if (cp > max_codepoint)
            continue;
        lo = any ? std::min(lo, cp) : cp;
        hi = any ? std::max(hi, cp) : cp;
        any = true;
    }
    if (!any)
        return false;
    first_codepoint = lo;
    line_height = SaturatingCast<u32>(std::max(1.0, PyRound(font.cap_line)));
    glyphs.assign(hi - lo + 1, EdenDsmodFontGlyph{});
    const i32 ah = font.atlas_h > 0 ? font.atlas_h : static_cast<i32>(font.atlas_image.height);
    for (u32 cp = lo; cp <= hi; ++cp) {
        const TmpGlyph* g = font.Find(cp);
        if (!g)
            continue;
        EdenDsmodFontGlyph& h = glyphs[cp - lo];
        h.x = static_cast<u16>(std::clamp(g->rect_x, 0, 65535));
        h.y = static_cast<u16>(std::clamp<i64>(i64{ah} - g->rect_y - g->rect_h, 0, 65535));
        h.w = static_cast<u16>(std::clamp(g->rect_w, 0, 65535));
        h.h = static_cast<u16>(std::clamp(g->rect_h, 0, 65535));
        h.bearing_x = SaturatingCast<std::int16_t>(PyRound(g->bearing_x));
        h.bearing_y = SaturatingCast<std::int16_t>(PyRound(g->bearing_y));
        h.advance = SaturatingCast<u16>(std::max(0.0, PyRound(g->advance)));
    }
    return true;
}

// ---- stats -------------------------------------------------------------------------------------

Stats GetStats() {
    Stats s;
    s.romfs_reads = g_stats.romfs_reads.load(std::memory_order_relaxed);
    s.romfs_bytes = g_stats.romfs_bytes.load(std::memory_order_relaxed);
    s.blocks_decompressed = g_stats.blocks_decompressed.load(std::memory_order_relaxed);
    s.bytes_decompressed = g_stats.bytes_decompressed.load(std::memory_order_relaxed);
    s.bundles_opened = g_stats.bundles_opened.load(std::memory_order_relaxed);
    s.objects_indexed = g_stats.objects_indexed.load(std::memory_order_relaxed);
    return s;
}

void SetBlockCacheBudget(std::size_t bytes) {
    g_block_budget.store(bytes, std::memory_order_relaxed);
}

void ResetStats() {
    g_stats.romfs_reads = 0;
    g_stats.romfs_bytes = 0;
    g_stats.blocks_decompressed = 0;
    g_stats.bytes_decompressed = 0;
    g_stats.bundles_opened = 0;
    g_stats.objects_indexed = 0;
}

} // namespace ce_unity
