// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Read access to guest memory and IL2CPP basic types (v31).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct EdenDsmodHostApi;

namespace cotl {

// Abstract guest memory: the host in the module, a FakeGuest in tests.
struct Guest {
    virtual ~Guest() = default;
    virtual bool Read(uint64_t addr, void* out, size_t n) const = 0;
};

// Reads through the host's is_mapped + read_memory.
class HostGuest : public Guest {
public:
    explicit HostGuest(const EdenDsmodHostApi* host) : host_(host) {}
    bool Read(uint64_t addr, void* out, size_t n) const override;

private:
    const EdenDsmodHostApi* host_;
};

// IL2CPP v31: Il2CppClass.static_fields, List<T>, Array, String.
inline constexpr uint64_t kClassStaticFields = 0xB8;
inline constexpr uint64_t kListItems = 0x10;
inline constexpr uint64_t kListSize = 0x18;
inline constexpr uint64_t kArrayLength = 0x18;
inline constexpr uint64_t kArrayData = 0x20;
inline constexpr uint64_t kStringLength = 0x10;
inline constexpr uint64_t kStringChars = 0x14;
inline constexpr uint32_t kMaxListSize = 512;

template <class T>
std::optional<T> ReadPod(const Guest& g, uint64_t addr) {
    T v{};
    if (addr == 0 || !g.Read(addr, &v, sizeof(T))) {
        return std::nullopt;
    }
    return v;
}

// Reads a pointer; null counts as absent.
std::optional<uint64_t> ReadPtr(const Guest& g, uint64_t addr);

// List<T> of object references. nullopt on a null list, invalid _size or _size > max_length.
std::optional<std::vector<uint64_t>> ReadRefList(const Guest& g, uint64_t list_obj,
                                                 uint32_t max = kMaxListSize);

// List<T> of value types (int, enum, float).
template <class T>
std::optional<std::vector<T>> ReadValueList(const Guest& g, uint64_t list_obj,
                                            uint32_t max = kMaxListSize);

// System.String to UTF-8. nullopt on a null object or a length outside [0, max_chars].
std::optional<std::string> ReadString(const Guest& g, uint64_t str_obj, uint32_t max_chars = 256);

// static_fields of a class through its TypeInfo slot in main.
std::optional<uint64_t> StaticFields(const Guest& g, uint64_t main_base, uint64_t typeinfo_off);

} // namespace cotl
