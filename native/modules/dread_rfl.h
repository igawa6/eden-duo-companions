// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Metroid Dread "standard format" (Mercury Engine reflection) reader: .bmmap (CMinimapData) and
// .brfld (gameeditor::CGameModelRoot), parsed exactly as mercury_engine_data_structures does.
//
// The encoding is not self-describing, so a value can only be skipped by knowing its type. The
// type layouts (field-name CRC64s, value kinds, pointer targets) are generated from MEDS's own
// constructs by tools/gen_dread_rfl_types.py into dread_rfl_types.inc; only the closure reachable
// from the two roots is included.
//
//   file   = u64 class crc, u16 major, u8 minor, u8 patch, u64 crc("Root"), Root, EOF
//   Object = u32 n, n x (u64 field-name crc64, value)        (unknown field -> error, as MEDS)
//   Vector = u32 n, n x value            Dict = u32 n, n x (key, value)
//   Pointer= u64 type crc, value of that type (the "void" crc -> null)
//   StrId  = NUL-terminated utf-8   Flag = 1 byte   Int/UInt/Float/Enum = 4   uint16 = 2
//   uint64 / property = 8           CVector2D/3D/4D = 2/3/4 floats   bytes = u32 n + n bytes
//
// Dicts follow Python dict semantics like MEDS' Container: a repeated key keeps its first
// position and takes the last value (Document::DuplicateKeys() counts them).
//
// The Document keeps string_views into the caller's file buffer: keep the buffer alive (and
// unmoved) for as long as the Document and its Values are used. Values are small handles.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dread_rfl {

// The engine's name hash (reflected CRC-64/ECMA, init ~0, no xorout); field names, type names,
// asset paths.
constexpr uint64_t Crc(std::string_view s) {
    uint64_t crc = ~uint64_t{0};
    for (const char ch : s) {
        crc ^= static_cast<uint8_t>(ch);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? uint64_t{0xC96C5795D7870F42} : 0);
    }
    return crc;
}

enum class Kind : uint8_t {
    None, // missing field / out of range / null pointer
    Str,
    Bool,
    I32,
    U32,
    F32,
    U16,
    U64,
    Prop, // a property (name) crc, u64
    Vec,  // CVector2D/3D/4D
    Bytes,
    Enum, // enum or flagset, u32
    Object,
    List,
    Dict,
    Pointer,
};

class Document;

// A handle to one node of a Document. Default-constructed / missing -> Kind::None.
// Container and vector accessors (size, [], key, value, field*, vec*) and Get/Find follow a
// Pointer to its pointee; kind() reports Kind::Pointer itself (use Deref().kind() for the pointee).
class Value {
public:
    Value() = default;
    Kind kind() const;
    bool valid() const {
        return kind() != Kind::None;
    }
    explicit operator bool() const {
        return valid();
    }

    // Scalars. as_int: I32 (sign-extended), U32/U16/U64/Prop/Enum/Bool as integers.
    float as_float() const; // F32 (0 otherwise)
    int64_t as_int() const;
    uint64_t as_uint() const;
    bool as_bool() const;
    std::string_view as_string() const;         // Str
    std::span<const uint8_t> as_bytes() const; // Bytes
    uint32_t float_bits() const;               // F32 raw bits

    // CVector2D/3D/4D
    size_t vec_size() const;
    float vec(size_t i) const;

    // List / Dict / Object entry count (dict after duplicate folding).
    size_t size() const;
    // List item i.
    Value operator[](size_t i) const;
    // Dict entry i (file order): key and value.
    Value key(size_t i) const;
    Value value(size_t i) const;
    // Object field i (file order): its name crc and value.
    uint64_t field_crc(size_t i) const;
    Value field(size_t i) const;

    // Object field by name (pointers are followed first), or Dict value by string key.
    Value Get(std::string_view name) const;
    Value GetCrc(uint64_t name_crc) const;
    // Dict value by string key / by integer key (u64/u32/prop keys).
    Value Find(std::string_view key) const;
    Value FindInt(uint64_t key) const;

    // Pointer: the pointed type crc (0 for a non-pointer), null-ness and the pointee.
    uint64_t type_crc() const;
    bool is_null_pointer() const;
    Value Deref() const; // a Pointer's pointee (None when null); any other value: itself

private:
    friend class Document;
    Value(const Document* d, uint32_t i) : doc_(d), index_(i) {}
    const Document* doc_{};
    uint32_t index_{};
};

class Document {
public:
    // Parse a whole standard-format file. `expected_class_crc` 0 accepts either supported root
    // (CMinimapData 1.0.2 / CScenario 49.0.2 game model); otherwise it must match.
    bool Parse(std::span<const uint8_t> file, uint64_t expected_class_crc, std::string* err);
    Value Root() const {
        return root_ == UINT32_MAX ? Value{} : Value{this, root_};
    }
    uint64_t ClassCrc() const {
        return class_crc_;
    }
    size_t NodeCount() const {
        return nodes_.size();
    }
    size_t MemoryBytes() const {
        return nodes_.capacity() * sizeof(Node) + floats_.capacity() * sizeof(float);
    }
    size_t DuplicateKeys() const {
        return duplicate_keys_;
    }
    size_t DuplicateFields() const {
        return duplicate_fields_;
    }

private:
    friend class Value;
    struct Node {
        Kind kind{Kind::None};
        uint8_t vn{};
        uint32_t a{}, b{};
        uint64_t u{};
    };
    struct Parser;
    std::span<const uint8_t> file_;
    std::vector<Node> nodes_;
    std::vector<float> floats_;
    uint32_t root_{UINT32_MAX};
    uint64_t class_crc_{};
    size_t duplicate_keys_{}, duplicate_fields_{};
};

// Free-function form of Document::Parse, the API the module uses.
inline bool ParseStandard(std::span<const uint8_t> file, uint64_t expected_class_crc_or_0,
                          Document& doc, std::string* err) {
    return doc.Parse(file, expected_class_crc_or_0, err);
}

constexpr uint64_t kClassMinimapData = Crc("CMinimapData");
constexpr uint64_t kClassScenario = Crc("CScenario");

} // namespace dread_rfl
