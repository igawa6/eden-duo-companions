// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Unity asset reader for the Luminescent Platinum companion. Format notes: lp_unity.h.

#include "lp_unity.h"
#include "lp_raster.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <unordered_map>

#include "bc_decoder.h"
#include "lz4.h"

// stb_truetype, private to this translation unit (static functions only). Contraction off so the
// rasteriser gives the same coverage on x86-64 and AArch64.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace lp_unity {
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
    std::vector<u64> min_bytes;  // per node: the fewest bytes one value of it can take (LinkTree)
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
    // A lower bound of each node's serialized size (alignment not counted), as ReadValue reads it:
    // an array count the remaining bytes cannot hold is refused before anything is allocated.
    // Children always follow their parent, so one pass from the end sees them first.
    t.min_bytes.assign(t.nodes.size(), 0);
    for (std::size_t i = t.nodes.size(); i-- > 0;) {
        const TreeNode& n = t.nodes[i];
        u64 size = 0;
        switch (n.prim) {
        case Prim::S8: case Prim::U8: case Prim::Bool: size = 1; break;
        case Prim::S16: case Prim::U16: size = 2; break;
        case Prim::S32: case Prim::U32: case Prim::F32: case Prim::String: case Prim::Typeless: size = 4; break;
        case Prim::S64: case Prim::U64: case Prim::F64: size = 8; break;
        case Prim::None:
            // the same order of cases as ReadValue: pair, array (just its count: it may be empty),
            // object; an Array node itself is a count too
            if ((n.type == "pair" && n.children.size() == 2) ||
                (n.type != "Array" && (n.children.empty() ||
                                       t.nodes[static_cast<std::size_t>(n.children[0])].type != "Array"))) {
                for (const int k : n.children)
                    size = std::min<u64>(size + t.min_bytes[static_cast<std::size_t>(k)], u64{1} << 40);
            } else {
                size = 4;
            }
            break;
        }
        t.min_bytes[i] = size;
    }
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
            const u64 element = static_cast<std::size_t>(sub) < t.min_bytes.size() ? t.min_bytes[static_cast<std::size_t>(sub)] : 0;
            if (!c.ok || count < 0 || static_cast<std::size_t>(count) > c.Remaining() ||
                (!IsByteElement(subnode) && (static_cast<std::size_t>(count) > budget ||
                                             (element > 0 && static_cast<u64>(count) > c.Remaining() / element))))
                return false;
            if (IsByteElement(subnode)) {
                if (!c.Bytes(out.s, static_cast<std::size_t>(count)))
                    return false;
                out.kind = Value::Kind::Bytes;
                if (subnode.meta & 0x4000)
                    c.Align();
            } else {
                // Grown as the values are read, so a damaged count cannot allocate ahead of the data.
                out.kind = Value::Kind::Array;
                out.items.clear();
                out.items.reserve(std::min<std::size_t>(static_cast<std::size_t>(count), 1024));
                const std::size_t start = c.pos;
                for (i32 k = 0; k < count; ++k) {
                    out.items.emplace_back();
                    if (!ReadValue(t, sub, c, out.items.back(), depth + 1, budget))
                        return false;
                    // elements that take no bytes: a long run of them is damage, not data
                    if (k == 0 && c.pos == start && count > 4096)
                        return false;
                }
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
        int n = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9')
            n = n * 10 + (v[i++] - '0');
        parts[k++] = n;
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
        cache.emplace_front(index, out);
        cached_bytes += out->size();
        while (cache.size() > 1 && cached_bytes > Bundle::BlockCacheBytes) {
            cached_bytes -= cache.back().second->size();
            cache.pop_back();
        }
        return out;
    }
};

Bundle::Bundle() : impl(std::make_unique<Impl>()) {}

std::size_t Bundle::CachedBytes() const {
    std::lock_guard lock{impl->mutex};
    return impl->cached_bytes;
}
Bundle::~Bundle() = default;

std::shared_ptr<Bundle> Bundle::Open(ByteSource file) {
    try {
        if (!file.read || file.size < 48)
            return nullptr;
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
        return b;
    } catch (...) {
        return nullptr;
    }
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
    if (limit <= PreloadBytes) {
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
    std::unordered_map<i64, std::size_t> by_id;
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
            m.by_id.emplace(o.path_id, m.objects.size());
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
        m.file = std::move(file);
        return sf;
    } catch (...) {
        return nullptr;
    }
}

std::int32_t SerializedFile::Platform() const {
    return impl->platform;
}
const std::vector<ObjectInfo>& SerializedFile::Objects() const {
    return impl->objects;
}
const ObjectInfo* SerializedFile::Find(std::int64_t path_id) const {
    const auto it = impl->by_id.find(path_id);
    return it == impl->by_id.end() ? nullptr : &impl->objects[it->second];
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
    if (!lp_raster::Fits(img.width, img.height, img.rgba.size()))
        return;
    if (mesh.verts.empty()) { // no polygon covers anything (and there is no verts[0] to start from)
        std::fill(img.rgba.begin(), img.rgba.end(), u8{0});
        return;
    }
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
            // C (int) cast; NaN / infinite / huge mesh values saturate instead of being undefined
            xy[k * 2] = lp_raster::TruncCoord((v[0] - min_x) * pixels_to_units);
            xy[k * 2 + 1] = lp_raster::TruncCoord((v[1] - min_y) * pixels_to_units);
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
    std::shared_ptr<SerializedFile> file;
    u64 resident{}; // bytes of the serialized file kept in memory (NodeSource preload)
    std::mutex mutex;
    bool indexed{};
    std::vector<std::pair<std::string, i64>> sprites;  // object order
    std::vector<std::pair<std::string, i64>> textures; // object order
    std::unordered_map<std::string, i64> sprite_by_name;
    std::unordered_map<std::string, i64> texture_by_name;
    // SpriteAtlas render data per atlas path id, keyed by GUID bytes + second.
    std::map<i64, std::shared_ptr<std::map<std::string, AtlasEntry>>> atlases;
    std::vector<std::pair<std::string, i64>> atlas_names;

    void Index() {
        if (indexed)
            return;
        indexed = true;
        for (const ObjectInfo& o : file->Objects()) {
            if (o.class_id != ClassSprite && o.class_id != ClassTexture2D &&
                o.class_id != ClassSpriteAtlas)
                continue;
            auto name = file->PeekName(o);
            if (!name)
                continue;
            if (o.class_id == ClassSprite) {
                sprite_by_name.emplace(*name, o.path_id);
                sprites.emplace_back(std::move(*name), o.path_id);
            } else if (o.class_id == ClassTexture2D) {
                texture_by_name.emplace(*name, o.path_id);
                textures.emplace_back(std::move(*name), o.path_id);
            } else {
                atlas_names.emplace_back(std::move(*name), o.path_id);
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
        for (const BundleNode& n : loaded->bundle->Nodes()) {
            if (n.path.ends_with(".resS") || n.path.ends_with(".resource"))
                continue;
            loaded->file = SerializedFile::Open(loaded->bundle->NodeSource(n));
            if (loaded->file) {
                loaded->resident = n.size <= Bundle::PreloadBytes ? n.size : 0;
                break;
            }
        }
        if (!loaded->file)
            return nullptr;
        std::lock_guard lock{mutex};
        for (auto it = lru.begin(); it != lru.end(); ++it)
            if (it->first == path) // another thread opened it meanwhile: share that one
                return it->second;
        lru.emplace_front(path, loaded);
        // At most max_bundles, and (keeping the newest) at most CacheBudget bytes of preloaded
        // serialized files and decompressed blocks across all of them: Android's memory is shared
        // with the emulated game.
        const auto bytes = [this] {
            u64 total = 0;
            for (const auto& [name, l] : lru)
                total += l->resident + l->bundle->CachedBytes();
            return total;
        };
        while (lru.size() > std::max<std::size_t>(max_bundles, 1) || (lru.size() > 1 && bytes() > CacheBudget))
            lru.pop_back();
        return loaded;
    }

    /// Raw-orientation region of a texture object of this bundle.
    std::optional<Image> Region(Loaded& l, const PPtr& ptr, i64 x0, i64 y0, i64 x1, i64 y1) {
        if (ptr.file_id != 0)
            return std::nullopt; // texture in another serialized file: not used by this game
        const ObjectInfo* o = l.file->Find(ptr.path_id);
        if (!o || o->class_id != ClassTexture2D)
            return std::nullopt;
        const auto v = l.file->ReadObject(*o);
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
        return DecodeRegion(fetch, data_size, *tex, l.file->Platform(), Astc(), x0, y0, x1, y1);
    }

    std::optional<SpriteInfo> Sprite(Loaded& l, std::string_view name,
                                     std::optional<SpriteMesh>* mesh_out = nullptr) {
        std::lock_guard lock{l.mutex};
        l.Index();
        const auto it = l.sprite_by_name.find(std::string{name});
        if (it == l.sprite_by_name.end())
            return std::nullopt;
        const ObjectInfo* o = l.file->Find(it->second);
        if (!o)
            return std::nullopt;
        const auto v = l.file->ReadObject(*o);
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
        if (have_atlas) {
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

std::vector<std::string> ReadMessageTable(Reader& reader, std::string_view lang_bundle,
                                          std::string_view object_name, bool event_newlines, bool array_index) {
    try {
        const auto v = reader.ReadObjectByName(lang_bundle, object_name, 114);
        return v ? DecodeMessageTable(*v, event_newlines, array_index) : std::vector<std::string>{};
    } catch (...) {
        return {};
    }
}

std::vector<std::string> DecodeMessageTable(const Value& table, bool event_newlines, bool array_index) {
    std::vector<std::string> out;
    const Value* labels = table.Get("labelDataArray");
    if (!labels)
        return out;
    for (const Value& label : labels->items) {
        const Value* idx = label.Get(array_index ? "arrayIndex" : "labelIndex");
        const std::int64_t at = idx ? idx->AsInt(-1) : -1;
        if (at < 0 || at > 1000000)
            continue;
        if (out.size() <= static_cast<std::size_t>(at))
            out.resize(static_cast<std::size_t>(at) + 1);
        std::string text;
        if (const Value* words = label.Get("wordDataArray"))
            for (const Value& w : words->items) {
                if (const Value* str = w.Get("str"))
                    text += str->AsString();
                if (const Value* event = w.Get("eventID"); event_newlines && event && event->AsInt() == 1)
                    text += '\n';
            }
        if (event_newlines)
            out[static_cast<std::size_t>(at)] += text;
        else
            out[static_cast<std::size_t>(at)] = std::move(text);
    }
    return out;
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
        i64 id;
        {
            std::lock_guard lock{l->mutex};
            l->Index();
            const auto it = l->texture_by_name.find(std::string{texture});
            if (it == l->texture_by_name.end())
                return std::nullopt;
            id = it->second;
        }
        const ObjectInfo* o = l->file->Find(id);
        if (!o)
            return std::nullopt;
        const auto v = l->file->ReadObject(*o);
        const auto tex = v ? TextureOf(*v) : std::nullopt;
        if (!tex)
            return std::nullopt;
        auto img = impl->Region(*l, PPtr{0, id}, 0, 0, tex->width, tex->height);
        if (img)
            FlipVertical(*img);
        return img;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Image> Reader::LoadSprite(std::string_view bundle, std::string_view sprite) {
    try {
        const auto l = impl->Open(bundle);
        if (!l)
            return std::nullopt;
        std::optional<SpriteMesh> mesh;
        const auto s = impl->Sprite(*l, sprite, &mesh);
        if (!s)
            return std::nullopt;
        const PPtr alpha = s->alpha_texture;
        // PIL crop box with Python round() on the float rect (x + width summed in double).
        // A NaN / infinite / huge rect (damaged data) is refused before the integer conversion.
        const double rx = s->texture_rect.x, ry = s->texture_rect.y;
        const auto rx0 = lp_raster::RoundCoord(rx), ry0 = lp_raster::RoundCoord(ry);
        const auto rx1 = lp_raster::RoundCoord(rx + static_cast<double>(s->texture_rect.width));
        const auto ry1 = lp_raster::RoundCoord(ry + static_cast<double>(s->texture_rect.height));
        if (!rx0 || !ry0 || !rx1 || !ry1)
            return std::nullopt;
        const i64 x0 = *rx0, y0 = *ry0, x1 = *rx1, y1 = *ry1;
        auto img = impl->Region(*l, s->texture, x0, y0, x1, y1);
        if (!img)
            return std::nullopt;
        if (!alpha.IsNull()) {
            const auto a = impl->Region(*l, alpha, x0, y0, x1, y1);
            if (!a || a->rgba.size() < img->rgba.size())
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

std::string PokemonIconBundle(int species, int form, int gender, bool shiny) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "UIs/textures_mass/pokemon_l/pm%04d_%02d_%02d_%02d_l",
                  std::clamp(species, 0, 9999), std::clamp(form, 0, 99), std::clamp(gender, 0, 99),
                  shiny ? 1 : 0);
    return buf;
}

std::string PokemonIconSprite(int species, int form, int gender, bool shiny) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "pm%04d_%02d_%02d_%02d_L", std::clamp(species, 0, 9999),
                  std::clamp(form, 0, 99), std::clamp(gender, 0, 99), shiny ? 1 : 0);
    return buf;
}

std::string ItemIconSprite(int item_id) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "item_%04d", std::clamp(item_id, 0, 9999));
    return buf;
}

std::optional<Image> Reader::LoadPokemonIcon(int species, int form, int gender, bool shiny) {
    // Requested gender, then 00/01/02; then non-shiny (pokemon_l ships no shiny icons in
    // BDSP 1.3.0 or Luminescent 2.2); then form 00 as the last resort.
    const int order[4]{gender, 0, 1, 2};
    const int forms[2]{form, 0};
    const bool shinies[2]{shiny, false};
    for (int f = 0; f < (form != 0 ? 2 : 1); ++f)
        for (int s = 0; s < (shiny ? 2 : 1); ++s)
            for (int k = 0; k < 4; ++k) {
                const int g = order[k];
                if (k > 0 && g == gender)
                    continue;
                const std::string bundle = PokemonIconBundle(species, forms[f], g, shinies[s]);
                const auto path = impl->root + bundle;
                if (!impl->read || !impl->read.size(path))
                    continue;
                if (auto img = LoadSprite(bundle, PokemonIconSprite(species, forms[f], g, shinies[s])))
                    return img;
            }
    return std::nullopt;
}

std::optional<Image> Reader::LoadItemIcon(int item_id) {
    return LoadSprite(bundles::TextureMass, ItemIconSprite(item_id));
}

// ---- fonts -------------------------------------------------------------------------------------

namespace {

std::optional<std::vector<u8>> FontFrom(const SerializedFile& sf, std::string_view name) {
    for (const ObjectInfo& o : sf.Objects()) {
        if (o.class_id != ClassFont)
            continue;
        const auto n = sf.PeekName(o);
        if (!n || *n != name)
            continue;
        const auto v = sf.ReadObject(o);
        if (!v)
            return std::nullopt;
        const Value* data = v->Get("m_FontData");
        if (!data || data->s.empty())
            return std::nullopt;
        return std::vector<u8>(data->s.begin(), data->s.end());
    }
    return std::nullopt;
}

ByteSource FileSource(const RangeReader& read, std::string_view path) {
    ByteSource src;
    if (!read)
        return src;
    const auto size = read.size(path);
    if (!size)
        return src;
    src.size = *size;
    std::string p{path};
    RangeReader r = read;
    src.read = [r, p](u64 off, u8* out, std::size_t n) { return r.read(p, off, out, n); };
    return src;
}

} // namespace

std::optional<std::vector<std::uint8_t>> ExtractFontData(const RangeReader& read,
                                                         std::string_view assets_path,
                                                         std::string_view font_name) {
    try {
        const auto sf = SerializedFile::Open(FileSource(read, assets_path));
        if (!sf)
            return std::nullopt;
        return FontFrom(*sf, font_name);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::vector<std::uint8_t>> ExtractBundleFontData(Reader& reader, std::string_view bundle,
                                                               std::string_view font_name) {
    try {
        const auto v = reader.ReadObjectByName(bundle, font_name, ClassFont);
        const Value* data = v ? v->Get("m_FontData") : nullptr;
        if (!data || data->s.empty())
            return std::nullopt;
        return std::vector<std::uint8_t>(data->s.begin(), data->s.end());
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::string> ListFonts(const RangeReader& read, std::string_view assets_path) {
    try {
        const auto sf = SerializedFile::Open(FileSource(read, assets_path));
        std::vector<std::string> out;
        if (!sf)
            return out;
        for (const ObjectInfo& o : sf->Objects())
            if (o.class_id == ClassFont)
                if (auto n = sf->PeekName(o))
                    out.push_back(std::move(*n));
        return out;
    } catch (...) {
        return {};
    }
}

std::vector<std::uint32_t> DefaultCodepoints() {
    std::vector<u32> cps;
    for (u32 c = 0x20; c <= 0x7E; ++c)
        cps.push_back(c);
    for (u32 c = 0xA0; c <= 0xFF; ++c)
        cps.push_back(c);
    for (u32 c : {0x2013u, 0x2014u, 0x2018u, 0x2019u, 0x201Cu, 0x201Du, 0x2022u, 0x2026u, 0x2640u,
                  0x2642u})
        cps.push_back(c);
    // What the language mods' tables draw beyond Latin-1 (contracts/language-mods.md §4): French
    // Œ œ and the narrow no-break space before ! ? :, German „ ℃, and the game's ― → − ★ ■ ♥ ー
    // and ideographic space; U+E300 is the game's Pokédollar (none of the faces draws it: it stays
    // a blank cell, but asking for it up front spares a second decode for every language).
    for (u32 c : {0x0152u, 0x0153u, 0x2015u, 0x201Eu, 0x202Fu, 0x2103u, 0x2192u, 0x2212u, 0x2605u,
                  0x25A0u, 0x2665u, 0x3000u, 0x30FCu, 0xE300u})
        cps.push_back(c);
    return cps;
}

std::optional<FontAtlas> BuildFontAtlas(std::span<const std::uint8_t> font,
                                        std::uint32_t cap_height_px,
                                        std::span<const std::uint32_t> codepoints,
                                        std::uint32_t page_h,
                                        std::span<const std::uint8_t> fallback) {
    try {
        constexpr u32 MaxAtlas = 4096;
        constexpr u32 Pad = 1;
        if (font.size() < 12 || font.size() > MaxAlloc || cap_height_px < 4 || cap_height_px > 256 ||
            codepoints.empty())
            return std::nullopt;
        std::vector<u32> cps(codepoints.begin(), codepoints.end());
        std::sort(cps.begin(), cps.end());
        cps.erase(std::unique(cps.begin(), cps.end()), cps.end());
        if (cps.back() - cps.front() > 0x10000)
            return std::nullopt; // the glyph table is a dense run
        // A face and its scale: 'H' (else 'E', 'I', else 0.7 of the ascent) is cap_height_px tall.
        const auto open = [](std::span<const std::uint8_t> bytes, stbtt_fontinfo& f) {
            const int offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
            return offset >= 0 && stbtt_InitFont(&f, bytes.data(), offset) != 0;
        };
        const auto cap_of = [](const stbtt_fontinfo& f) {
            for (const int probe : {'H', 'E', 'I'}) {
                int x0, y0, x1, y1;
                const int gi = stbtt_FindGlyphIndex(&f, probe);
                if (gi != 0 && stbtt_GetGlyphBox(&f, gi, &x0, &y0, &x1, &y1) && y1 > 0)
                    return static_cast<float>(y1);
            }
            int a, d, g;
            stbtt_GetFontVMetrics(&f, &a, &d, &g);
            return static_cast<float>(a) * 0.7f;
        };
        stbtt_fontinfo info{};
        if (!open(font, info))
            return std::nullopt;
        const float cap_units = cap_of(info);
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
        if (cap_units <= 0.0f)
            return std::nullopt;
        const float scale = static_cast<float>(cap_height_px) / cap_units;
        // CJK glyphs at their own height and centre (CjkMetricsFor), measured on the face's own
        // ideograph: 国 (Chinese, Japanese), 한 (Korean) or あ.
        CjkMetrics cjk{scale, 0};
        for (const int probe : {0x56FD, 0xD55C, 0x3042}) {
            int x0, y0, x1, y1;
            const int gi = stbtt_FindGlyphIndex(&info, probe);
            if (gi != 0 && stbtt_GetGlyphBox(&info, gi, &x0, &y0, &x1, &y1) && y1 > y0) {
                cjk = CjkMetricsFor(y0, y1, cap_height_px, scale);
                break;
            }
        }
        // Glyphs the face lacks come from the fallback face at the same cap height (a Chinese face
        // without accented Latin letters, for the English item names LP adds in every language).
        stbtt_fontinfo fb{};
        const bool has_fb = fallback.size() >= 12 && fallback.size() <= MaxAlloc && open(fallback, fb) && cap_of(fb) > 0.0f;
        const float fb_scale = has_fb ? static_cast<float>(cap_height_px) / cap_of(fb) : 0.0f;

        struct Raster {
            u32 cp{};
            int w{}, h{}, x0{}, y0{}, advance{}, shift{};
            std::vector<u8> coverage;
            u32 ax{}, ay{};
        };
        std::vector<Raster> rasters;
        FontAtlas out;
        for (const u32 cp : cps) {
            const stbtt_fontinfo* face = &info;
            float sc = scale;
            int gi = stbtt_FindGlyphIndex(&info, static_cast<int>(cp));
            // Accented Latin letters (U+00A0-U+024F) come from the fallback face first: the
            // Chinese faces draw them full-width ("Pok é dex").
            const bool latin_ext = cp >= 0xA0 && cp <= 0x24F;
            if (has_fb && cp != ' ' && (gi == 0 || latin_ext)) {
                if (const int fgi = stbtt_FindGlyphIndex(&fb, static_cast<int>(cp)); fgi != 0) {
                    face = &fb;
                    sc = fb_scale;
                    gi = fgi;
                }
            }
            if (gi == 0) {
                out.missing.push_back(cp);
                continue;
            }
            Raster r;
            r.cp = cp;
            if (face == &info && CjkCentred(cp)) {
                sc = cjk.scale;
                r.shift = cjk.shift;
            }
            int adv, lsb, x1, y1;
            stbtt_GetGlyphHMetrics(face, gi, &adv, &lsb);
            r.advance = static_cast<int>(std::lround(static_cast<float>(adv) * sc));
            stbtt_GetGlyphBitmapBox(face, gi, sc, sc, &r.x0, &r.y0, &x1, &y1);
            r.w = std::max(0, x1 - r.x0);
            r.h = std::max(0, y1 - r.y0);
            if (r.w > 0 && r.h > 0) {
                if (r.w > 1024 || r.h > 1024)
                    return std::nullopt;
                r.coverage.assign(static_cast<std::size_t>(r.w) * r.h, 0);
                stbtt_MakeGlyphBitmap(face, r.coverage.data(), r.w, r.h, r.w, sc, sc, gi);
                if (std::all_of(r.coverage.begin(), r.coverage.end(), [](u8 v) { return v == 0; })) {
                    r.w = r.h = 0;
                    r.coverage.clear();
                }
            } else {
                r.w = r.h = 0;
            }
            rasters.push_back(std::move(r));
        }
        // Shelf packing. Paged (page_h > 0): a shelf never straddles a page boundary, and past
        // one page the atlas stays PagedWidth wide and grows by whole pages (the host loads each
        // page as its own image, so every page must stay a valid image on its own).
        constexpr u32 PagedWidth = 2048;
        u32 width = 0, height = 0;
        for (u32 w = 128; w <= MaxAtlas; w *= 2) {
            u32 x = 0, y = 0, shelf = 0;
            bool fits = true;
            for (auto& r : rasters) {
                if (r.w == 0)
                    continue;
                const u32 cw = static_cast<u32>(r.w) + 2 * Pad, ch = static_cast<u32>(r.h) + 2 * Pad;
                if (cw > w || (page_h > 0 && ch > page_h)) {
                    fits = false;
                    break;
                }
                if (x + cw > w) {
                    x = 0;
                    y += shelf;
                    shelf = 0;
                }
                if (page_h > 0 && y / page_h != (y + ch - 1) / page_h) {
                    x = 0; // the shelf would cross into the next page: start it there
                    y = (y / page_h + 1) * page_h;
                    shelf = 0;
                }
                r.ax = x + Pad;
                r.ay = y + Pad;
                x += cw;
                shelf = std::max(shelf, ch);
            }
            const bool square = y + shelf <= w && (page_h == 0 || y + shelf <= page_h);
            const bool paged = page_h > 0 && w == PagedWidth && y + shelf <= 0xFFFF;
            if (fits && (square || paged)) {
                width = w;
                height = std::max<u32>(y + shelf, 1);
                break;
            }
        }
        if (width == 0 || static_cast<std::size_t>(width) * height * 4 > MaxAlloc)
            return std::nullopt;
        out.page_h = page_h;
        out.atlas.width = width;
        out.atlas.height = height;
        out.atlas.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
        for (std::size_t i = 0; i < out.atlas.rgba.size(); i += 4)
            out.atlas.rgba[i] = out.atlas.rgba[i + 1] = out.atlas.rgba[i + 2] = 0xFF;
        for (const auto& r : rasters)
            for (int y = 0; y < r.h; ++y)
                for (int x = 0; x < r.w; ++x)
                    out.atlas.rgba[((static_cast<std::size_t>(r.ay) + y) * width + r.ax + x) * 4 + 3] =
                        r.coverage[static_cast<std::size_t>(y) * r.w + x];
        const auto glyph_of = [](const Raster& r) {
            EdenDsmodFontGlyph g{};
            if (r.w > 0) {
                g.x = static_cast<u16>(r.ax);
                g.y = static_cast<u16>(r.ay);
                g.w = static_cast<u16>(r.w);
                g.h = static_cast<u16>(r.h);
                g.bearing_x = static_cast<std::int16_t>(r.x0);
                g.bearing_y = static_cast<std::int16_t>(-r.y0 - r.shift);
            }
            g.advance = static_cast<u16>(std::max(0, r.advance));
            return g;
        };
        EdenDsmodFontGlyph blank{};
        blank.advance = static_cast<u16>((cap_height_px + 1) / 2);
        for (const auto& r : rasters)
            if (r.cp == ' ')
                blank.advance = glyph_of(r).advance;
        out.first_codepoint = cps.front();
        out.cjk = cjk;
        out.line_height = cap_height_px;
        out.scale = scale;
        out.ascent_px = static_cast<i32>(std::lround(static_cast<float>(ascent) * scale));
        out.descent_px = static_cast<i32>(std::lround(static_cast<float>(descent) * scale));
        out.glyphs.assign(cps.back() - cps.front() + 1, blank);
        for (const auto& r : rasters)
            out.glyphs[r.cp - out.first_codepoint] = glyph_of(r);
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace lp_unity
