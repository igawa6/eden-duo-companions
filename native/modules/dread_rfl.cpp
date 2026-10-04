// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dread_rfl.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace dread_rfl {
namespace {

enum class TypeKind : uint8_t {
    Str,
    Bool,
    I32,
    U32,
    F32,
    U16,
    U64,
    Prop,
    Vec2,
    Vec3,
    Vec4,
    Bytes,
    Enum,
    Object,
    Vector,
    Dict,
    Pointer,
};
struct TypeDesc {
    TypeKind kind;
    uint32_t a; // Object: first field, Vector: element type, Dict: key type, Pointer: first option
    uint32_t b; // Object: field count, Dict: value type, Pointer: option count
};
struct FieldDesc {
    uint64_t crc;
    uint32_t type;
};
constexpr uint32_t kVoid = UINT32_MAX;
struct PtrDesc {
    uint64_t crc;
    uint32_t type; // kVoid: null
};

#include "dread_rfl_types.inc"

constexpr uint32_t kTypeCount = sizeof(kTypes) / sizeof(kTypes[0]);

struct VersionedRoot {
    uint64_t class_crc;
    uint16_t major;
    uint8_t minor, patch;
    uint32_t root_type;
};
constexpr VersionedRoot kRoots[] = {
    {kClassMinimapData, 1, 0, 2, kRootMinimapData},
    {kClassScenario, 49, 0, 2, kRootGameModelRoot},
};

constexpr uint32_t kMaxDepth = 256;

} // namespace

struct Document::Parser {
    Document& d;
    const uint8_t* p;
    const uint8_t* end;
    std::string* err;
    uint32_t depth{};

    bool Fail(const char* what) {
        if (err)
            *err = std::string{what} + " at offset " +
                   std::to_string(static_cast<size_t>(p - d.file_.data()));
        return false;
    }
    bool Need(size_t n) {
        return static_cast<size_t>(end - p) >= n || Fail("truncated");
    }
    template <class T>
    bool Take(T& v) {
        if (!Need(sizeof(T)))
            return false;
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return true;
    }
    uint32_t Alloc(size_t n) {
        const size_t at = d.nodes_.size();
        d.nodes_.resize(at + n);
        return static_cast<uint32_t>(at);
    }

    // Parse one value of `type` into node slot `slot` (already allocated).
    bool Value(uint32_t type, uint32_t slot) {
        if (type >= kTypeCount)
            return Fail("bad type index");
        const TypeDesc& t = kTypes[type];
        Node n;
        switch (t.kind) {
        case TypeKind::Str: {
            const auto* z = static_cast<const uint8_t*>(std::memchr(p, 0, size_t(end - p)));
            if (!z)
                return Fail("unterminated string");
            n.kind = Kind::Str;
            n.a = static_cast<uint32_t>(p - d.file_.data());
            n.b = static_cast<uint32_t>(z - p);
            p = z + 1;
            break;
        }
        case TypeKind::Bool: {
            uint8_t v;
            if (!Take(v))
                return false;
            n.kind = Kind::Bool;
            n.u = v != 0;
            break;
        }
        case TypeKind::I32: {
            int32_t v;
            if (!Take(v))
                return false;
            n.kind = Kind::I32;
            n.u = static_cast<uint64_t>(static_cast<int64_t>(v));
            break;
        }
        case TypeKind::U32:
        case TypeKind::Enum:
        case TypeKind::F32: {
            uint32_t v;
            if (!Take(v))
                return false;
            n.kind = t.kind == TypeKind::U32    ? Kind::U32
                     : t.kind == TypeKind::Enum ? Kind::Enum
                                                : Kind::F32;
            n.u = v;
            break;
        }
        case TypeKind::U16: {
            uint16_t v;
            if (!Take(v))
                return false;
            n.kind = Kind::U16;
            n.u = v;
            break;
        }
        case TypeKind::U64:
        case TypeKind::Prop: {
            uint64_t v;
            if (!Take(v))
                return false;
            n.kind = t.kind == TypeKind::U64 ? Kind::U64 : Kind::Prop;
            n.u = v;
            break;
        }
        case TypeKind::Vec2:
        case TypeKind::Vec3:
        case TypeKind::Vec4: {
            const uint8_t count = t.kind == TypeKind::Vec2 ? 2 : t.kind == TypeKind::Vec3 ? 3 : 4;
            if (!Need(size_t(count) * 4))
                return false;
            n.kind = Kind::Vec;
            n.vn = count;
            n.a = static_cast<uint32_t>(d.floats_.size());
            for (uint8_t i = 0; i < count; ++i) {
                float f;
                std::memcpy(&f, p, 4);
                p += 4;
                d.floats_.push_back(f);
            }
            break;
        }
        case TypeKind::Bytes: {
            uint32_t size;
            if (!Take(size) || !Need(size))
                return false;
            n.kind = Kind::Bytes;
            n.a = static_cast<uint32_t>(p - d.file_.data());
            n.b = size;
            p += size;
            break;
        }
        case TypeKind::Object: {
            uint32_t count;
            if (!Take(count))
                return false;
            if (count > static_cast<size_t>(end - p) / 8)
                return Fail("object field count");
            if (++depth > kMaxDepth)
                return Fail("too deep");
            const uint32_t first = Alloc(size_t(count) * 2);
            const FieldDesc* fb = kFields + t.a;
            const FieldDesc* fe = fb + t.b;
            for (uint32_t i = 0; i < count; ++i) {
                uint64_t crc;
                if (!Take(crc))
                    return false;
                const FieldDesc* f = std::lower_bound(
                    fb, fe, crc, [](const FieldDesc& x, uint64_t c) { return x.crc < c; });
                if (f == fe || f->crc != crc)
                    return Fail("unknown field");
                for (uint32_t j = 0; j < i; ++j)
                    if (d.nodes_[first + 2 * j].u == crc) {
                        ++d.duplicate_fields_;
                        break;
                    }
                d.nodes_[first + 2 * i].kind = Kind::Prop;
                d.nodes_[first + 2 * i].u = crc;
                if (!Value(f->type, first + 2 * i + 1))
                    return false;
            }
            --depth;
            n.kind = Kind::Object;
            n.a = first;
            n.b = count;
            break;
        }
        case TypeKind::Vector: {
            uint32_t count;
            if (!Take(count))
                return false;
            if (count > static_cast<size_t>(end - p))
                return Fail("vector count");
            if (++depth > kMaxDepth)
                return Fail("too deep");
            const uint32_t first = Alloc(count);
            for (uint32_t i = 0; i < count; ++i)
                if (!Value(t.a, first + i))
                    return false;
            --depth;
            n.kind = Kind::List;
            n.a = first;
            n.b = count;
            break;
        }
        case TypeKind::Dict: {
            uint32_t count;
            if (!Take(count))
                return false;
            if (count > static_cast<size_t>(end - p))
                return Fail("dict count");
            if (++depth > kMaxDepth)
                return Fail("too deep");
            const uint32_t first = Alloc(size_t(count) * 2);
            for (uint32_t i = 0; i < count; ++i)
                if (!Value(t.a, first + 2 * i) || !Value(t.b, first + 2 * i + 1))
                    return false;
            --depth;
            n.kind = Kind::Dict;
            n.a = first;
            n.b = FoldDuplicates(first, count);
            break;
        }
        case TypeKind::Pointer: {
            uint64_t crc;
            if (!Take(crc))
                return false;
            const PtrDesc* ob = kPtrOpts + t.a;
            const PtrDesc* oe = ob + t.b;
            const PtrDesc* o = std::lower_bound(
                ob, oe, crc, [](const PtrDesc& x, uint64_t c) { return x.crc < c; });
            if (o == oe || o->crc != crc)
                return Fail("pointer type not allowed here");
            n.kind = Kind::Pointer;
            n.u = crc;
            n.a = UINT32_MAX;
            if (o->type != kVoid) {
                const uint32_t child = Alloc(1);
                if (++depth > kMaxDepth)
                    return Fail("too deep");
                if (!Value(o->type, child))
                    return false;
                --depth;
                n.a = child;
            }
            break;
        }
        }
        d.nodes_[slot] = n;
        return true;
    }

    // Python dict semantics for the (key, value) pairs at [first, first + 2 * count): a repeated
    // key keeps its first position and takes the later value. Returns the folded count.
    uint32_t FoldDuplicates(uint32_t first, uint32_t count) {
        if (count < 2)
            return count;
        const auto key_of = [&](uint32_t i) -> std::pair<uint64_t, std::string_view> {
            const Node& k = d.nodes_[first + 2 * i];
            if (k.kind == Kind::Str)
                return {0, std::string_view{reinterpret_cast<const char*>(d.file_.data()) + k.a,
                                            k.b}};
            return {k.u + (uint64_t(k.kind) << 56), {}};
        };
        struct Hash {
            size_t operator()(const std::pair<uint64_t, std::string_view>& k) const {
                return std::hash<std::string_view>{}(k.second) ^ std::hash<uint64_t>{}(k.first);
            }
        };
        std::unordered_map<std::pair<uint64_t, std::string_view>, uint32_t, Hash> seen;
        seen.reserve(count);
        uint32_t out = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const auto key = key_of(i);
            const auto [it, fresh] = seen.try_emplace(key, out);
            if (fresh) {
                if (out != i) {
                    d.nodes_[first + 2 * out] = d.nodes_[first + 2 * i];
                    d.nodes_[first + 2 * out + 1] = d.nodes_[first + 2 * i + 1];
                }
                ++out;
            } else {
                d.nodes_[first + 2 * it->second + 1] = d.nodes_[first + 2 * i + 1];
                ++d.duplicate_keys_;
            }
        }
        return out;
    }
};

bool Document::Parse(std::span<const uint8_t> file, uint64_t expected_class_crc,
                     std::string* err) {
    file_ = file;
    nodes_.clear();
    floats_.clear();
    root_ = UINT32_MAX;
    duplicate_keys_ = duplicate_fields_ = 0;
    nodes_.reserve(file.size() / 10 + 16);
    floats_.reserve(file.size() / 24 + 16);
    Parser ps{*this, file.data(), file.data() + file.size(), err};
    uint64_t class_crc, root_name;
    uint16_t major;
    uint8_t minor, patch;
    if (!ps.Take(class_crc) || !ps.Take(major) || !ps.Take(minor) || !ps.Take(patch) ||
        !ps.Take(root_name))
        return false;
    if (expected_class_crc != 0 && class_crc != expected_class_crc)
        return ps.Fail("unexpected class");
    const VersionedRoot* root = nullptr;
    for (const auto& r : kRoots)
        if (r.class_crc == class_crc)
            root = &r;
    if (!root)
        return ps.Fail("unsupported class");
    if (major != root->major || minor != root->minor || patch != root->patch)
        return ps.Fail("unsupported version");
    if (root_name != Crc("Root"))
        return ps.Fail("missing Root");
    const uint32_t slot = ps.Alloc(1);
    if (!ps.Value(root->root_type, slot))
        return false;
    if (ps.p != ps.end)
        return ps.Fail("trailing bytes");
    class_crc_ = class_crc;
    root_ = slot;
    return true;
}

// ------------------------------------------------------------------------------------ Value

Kind Value::kind() const {
    return doc_ && index_ < doc_->nodes_.size() ? doc_->nodes_[index_].kind : Kind::None;
}
float Value::as_float() const {
    if (kind() != Kind::F32)
        return 0.0f;
    const uint32_t bits = static_cast<uint32_t>(doc_->nodes_[index_].u);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
uint32_t Value::float_bits() const {
    return kind() == Kind::F32 ? static_cast<uint32_t>(doc_->nodes_[index_].u) : 0;
}
int64_t Value::as_int() const {
    switch (kind()) {
    case Kind::I32:
    case Kind::Bool:
    case Kind::U32:
    case Kind::U16:
    case Kind::U64:
    case Kind::Prop:
    case Kind::Enum:
        return static_cast<int64_t>(doc_->nodes_[index_].u);
    default:
        return 0;
    }
}
uint64_t Value::as_uint() const {
    return static_cast<uint64_t>(as_int());
}
bool Value::as_bool() const {
    return as_int() != 0;
}
std::string_view Value::as_string() const {
    if (kind() != Kind::Str)
        return {};
    const auto& n = doc_->nodes_[index_];
    return {reinterpret_cast<const char*>(doc_->file_.data()) + n.a, n.b};
}
std::span<const uint8_t> Value::as_bytes() const {
    if (kind() != Kind::Bytes)
        return {};
    const auto& n = doc_->nodes_[index_];
    return doc_->file_.subspan(n.a, n.b);
}
size_t Value::vec_size() const {
    if (kind() == Kind::Pointer)
        return Deref().vec_size();
    return kind() == Kind::Vec ? doc_->nodes_[index_].vn : 0;
}
float Value::vec(size_t i) const {
    if (kind() == Kind::Pointer)
        return Deref().vec(i);
    return i < vec_size() ? doc_->floats_[doc_->nodes_[index_].a + i] : 0.0f;
}
size_t Value::size() const {
    if (kind() == Kind::Pointer)
        return Deref().size();
    const Kind k = kind();
    return k == Kind::List || k == Kind::Dict || k == Kind::Object ? doc_->nodes_[index_].b : 0;
}
Value Value::operator[](size_t i) const {
    if (kind() == Kind::Pointer)
        return Deref()[i];
    if (kind() != Kind::List || i >= size())
        return {};
    return {doc_, static_cast<uint32_t>(doc_->nodes_[index_].a + i)};
}
Value Value::key(size_t i) const {
    if (kind() == Kind::Pointer)
        return Deref().key(i);
    if (kind() != Kind::Dict || i >= size())
        return {};
    return {doc_, static_cast<uint32_t>(doc_->nodes_[index_].a + 2 * i)};
}
Value Value::value(size_t i) const {
    if (kind() == Kind::Pointer)
        return Deref().value(i);
    if (kind() != Kind::Dict || i >= size())
        return {};
    return {doc_, static_cast<uint32_t>(doc_->nodes_[index_].a + 2 * i + 1)};
}
uint64_t Value::field_crc(size_t i) const {
    if (kind() == Kind::Pointer)
        return Deref().field_crc(i);
    if (kind() != Kind::Object || i >= size())
        return 0;
    return doc_->nodes_[doc_->nodes_[index_].a + 2 * i].u;
}
Value Value::field(size_t i) const {
    if (kind() == Kind::Pointer)
        return Deref().field(i);
    if (kind() != Kind::Object || i >= size())
        return {};
    return {doc_, static_cast<uint32_t>(doc_->nodes_[index_].a + 2 * i + 1)};
}
Value Value::GetCrc(uint64_t name_crc) const {
    const Value v = Deref();
    if (v.kind() != Kind::Object)
        return {};
    // the last occurrence, like MEDS' Container for a repeated field
    for (size_t i = v.size(); i-- > 0;)
        if (v.field_crc(i) == name_crc)
            return v.field(i);
    return {};
}
Value Value::Get(std::string_view name) const {
    const Value v = Deref();
    if (v.kind() == Kind::Dict)
        return v.Find(name);
    return v.GetCrc(Crc(name));
}
Value Value::Find(std::string_view key_text) const {
    const Value v = Deref();
    if (v.kind() != Kind::Dict)
        return {};
    for (size_t i = 0; i < v.size(); ++i)
        if (v.key(i).kind() == Kind::Str && v.key(i).as_string() == key_text)
            return v.value(i);
    return {};
}
Value Value::FindInt(uint64_t key_value) const {
    const Value v = Deref();
    if (v.kind() != Kind::Dict)
        return {};
    for (size_t i = 0; i < v.size(); ++i) {
        const Value k = v.key(i);
        if (k.kind() != Kind::Str && k.kind() != Kind::None && k.as_uint() == key_value)
            return v.value(i);
    }
    return {};
}
uint64_t Value::type_crc() const {
    return kind() == Kind::Pointer ? doc_->nodes_[index_].u : 0;
}
bool Value::is_null_pointer() const {
    return kind() == Kind::Pointer && doc_->nodes_[index_].a == UINT32_MAX;
}
Value Value::Deref() const {
    if (kind() != Kind::Pointer)
        return *this;
    const uint32_t child = doc_->nodes_[index_].a;
    return child == UINT32_MAX ? Value{} : Value{doc_, child};
}

} // namespace dread_rfl
