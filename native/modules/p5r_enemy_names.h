// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace p5r_enemy_names {
struct Result {
    bool ready{}, torn{}, unsupported{}, persona{}, forced_alias{};
    std::string name;
};
// D4B1 native 6474E0/87FE90 contract. Caller must validate active enemy ownership.
// Failure always returns no name; no offline identity fallback is performed.
template <class Read>
Result ReadName(Read&& read, std::uint64_t main, std::uint32_t id) {
    Result out;
    struct Sample {
        std::uint64_t address{};
        std::size_t size{};
        std::array<std::uint8_t, 16> bytes{};
    };
    std::array<Sample, 160> journal{};
    std::size_t samples = 0;
    auto address = [](std::uint64_t base, std::uint64_t offset, std::uint64_t& a) {
        if (base < 0x10000 || base > 0x7fffffffffULL || offset > 0x7fffffffffULL - base)
            return false;
        a = base + offset;
        return true;
    };
    auto get = [&](std::uint64_t base, std::uint64_t offset, auto& value) {
        std::uint64_t a{};
        if (!address(base, offset, a) || sizeof(value) - 1 > 0x7fffffffffULL - a)
            return false;
        static_assert(sizeof(value) <= 16);
        if (samples >= journal.size())
            return false;
        auto& s = journal[samples];
        s.address = a;
        s.size = sizeof(value);
        if (!read(a, s.bytes.data(), s.size))
            return false;
        std::memcpy(&value, s.bytes.data(), sizeof(value));
        ++samples;
        return true;
    };
    if (id >= 783)
        return out;
    std::uint64_t flags{};
    std::uint32_t word{};
    if (!get(main, 0x1d834f8, flags) || !get(flags, 0x20, word))
        return out;
    out.forced_alias = (word & (1u << 28)) != 0;
    std::uint32_t name_id = id;
    if (!out.forced_alias && (id <= 160 || (id >= 424 && id <= 463))) {
        std::uint8_t known{}, definition{};
        if (!get(main, 0x2273210 + std::uint64_t(id) * 48, known))
            return out;
        if (known & 1) {
            std::uint64_t definitions{};
            if (!get(main, 0x220a160, definitions) ||
                !get(definitions, std::uint64_t(id) * 14, definition))
                return out;
            if (!(definition & 0x28)) {
                std::uint64_t mappings{};
                std::uint16_t mapped{};
                if (!get(main, 0x220a190, mappings) ||
                    !get(mappings, std::uint64_t(id) * 6, mapped))
                    return out;
                out.persona = true;
                name_id = mapped;
            }
        }
    }
    std::uint16_t count{}, offset{};
    std::array<std::uint64_t, 2> pair{};
    if (!get(main, out.persona ? 0x22ab51e : 0x22ab51c, count) || count == 0 ||
        count > (out.persona ? 464 : 783) || name_id >= count ||
        !get(main, out.persona ? 0x22ab628 : 0x22ab618, pair) ||
        !get(pair[1], std::uint64_t(name_id) * 2, offset))
        return Result{};
    bool ended = false;
    for (std::size_t i = 0; i < 128; i++) {
        std::uint8_t c{};
        if (!get(pair[0], std::uint64_t(offset) + i, c))
            return Result{};
        if (c == 0) {
            ended = true;
            break;
        }
        if (c < 32 || c >= 127) {
            out.unsupported = true;
            out.name.clear();
            return out;
        }
        out.name.push_back(static_cast<char>(c));
    }
    if (!ended || out.name.empty())
        return Result{};
    for (std::size_t i = 0; i < samples; i++) {
        const auto& s = journal[i];
        std::array<std::uint8_t, 16> after{};
        if (!read(s.address, after.data(), s.size) ||
            std::memcmp(after.data(), s.bytes.data(), s.size) != 0) {
            out.ready = false;
            out.torn = true;
            out.name.clear();
            return out;
        }
    }
    out.ready = true;
    return out;
}
} // namespace p5r_enemy_names
