// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_guest.h"

#include "core/mods/dsmod_module_abi.h"

namespace cotl {

bool HostGuest::Read(uint64_t addr, void* out, size_t n) const {
    if (host_ == nullptr || host_->read_memory == nullptr || addr == 0 || addr + n < addr) {
        return false;
    }
    // read_memory checks the address itself (mod_module_host.cpp)
    return host_->read_memory(host_->userdata, addr, out, n) != 0;
}

std::optional<uint64_t> ReadPtr(const Guest& g, uint64_t addr) {
    const auto p = ReadPod<uint64_t>(g, addr);
    if (!p || *p == 0) {
        return std::nullopt;
    }
    return p;
}

namespace {

// Returns the array pointer and element count of a List<T>, checked against max and the array length.
std::optional<std::pair<uint64_t, uint32_t>> ListHeader(const Guest& g, uint64_t list_obj,
                                                        uint32_t max) {
    if (list_obj == 0) {
        return std::nullopt;
    }
    const auto items = ReadPtr(g, list_obj + kListItems);
    const auto size = ReadPod<int32_t>(g, list_obj + kListSize);
    if (!items || !size || *size < 0 || static_cast<uint32_t>(*size) > max) {
        return std::nullopt;
    }
    const auto length = ReadPod<uint64_t>(g, *items + kArrayLength);
    if (!length || *length < static_cast<uint64_t>(*size)) {
        return std::nullopt;
    }
    return std::pair{*items, static_cast<uint32_t>(*size)};
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

} // namespace

std::optional<std::vector<uint64_t>> ReadRefList(const Guest& g, uint64_t list_obj, uint32_t max) {
    const auto header = ListHeader(g, list_obj, max);
    if (!header) {
        return std::nullopt;
    }
    std::vector<uint64_t> out(header->second);
    if (!out.empty() && !g.Read(header->first + kArrayData, out.data(), out.size() * 8)) {
        return std::nullopt;
    }
    return out;
}

template <class T>
std::optional<std::vector<T>> ReadValueList(const Guest& g, uint64_t list_obj, uint32_t max) {
    const auto header = ListHeader(g, list_obj, max);
    if (!header) {
        return std::nullopt;
    }
    std::vector<T> out(header->second);
    if (!out.empty() && !g.Read(header->first + kArrayData, out.data(), out.size() * sizeof(T))) {
        return std::nullopt;
    }
    return out;
}
template std::optional<std::vector<int32_t>> ReadValueList<int32_t>(const Guest&, uint64_t, uint32_t);

std::optional<std::string> ReadString(const Guest& g, uint64_t str_obj, uint32_t max_chars) {
    const auto len = ReadPod<int32_t>(g, str_obj + kStringLength);
    if (str_obj == 0 || !len || *len < 0 || static_cast<uint32_t>(*len) > max_chars) {
        return std::nullopt;
    }
    std::vector<uint16_t> units(static_cast<size_t>(*len));
    if (!units.empty() && !g.Read(str_obj + kStringChars, units.data(), units.size() * 2)) {
        return std::nullopt;
    }
    std::string out;
    for (size_t i = 0; i < units.size(); ++i) {
        uint32_t cp = units[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < units.size() && units[i + 1] >= 0xDC00 &&
            units[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            ++i;
        } else if (cp >= 0xD800 && cp < 0xE000) {
            cp = 0xFFFD;
        }
        AppendUtf8(out, cp);
    }
    return out;
}

std::optional<uint64_t> StaticFields(const Guest& g, uint64_t main_base, uint64_t typeinfo_off) {
    const auto klass = ReadPtr(g, main_base + typeinfo_off);
    if (!klass) {
        return std::nullopt;
    }
    return ReadPtr(g, *klass + kClassStaticFields);
}

} // namespace cotl
