// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bounds-checked GFLXPACK and FlatBuffers access for the player's encounter archives.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>
#include "lz4.h"
#include "core/mods/dsmod_module_abi.h"

namespace sw_archive {
using Bytes = std::vector<std::uint8_t>;
inline void Require(bool ok) { if (!ok) throw std::runtime_error("invalid encounter archive"); }
inline std::uint64_t Read(const Bytes& b, std::size_t p, unsigned n) {
    Require(n <= 8 && p <= b.size() && n <= b.size() - p);
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= std::uint64_t(b[p+i]) << (8*i);
    return v;
}
inline std::uint64_t Hash(std::string_view s) {
    std::uint64_t h = 0xCBF29CE484222645ull;
    for (unsigned char c : s) h = (h ^ c) * 0x100000001B3ull;
    return h;
}
inline Bytes Load(const EdenDsmodHostApi& host, const char* path) {
    Require(host.read_romfs);
    const auto n = host.read_romfs(host.userdata, path, 0, nullptr, 0);
    Require(n && n <= 32 * 1024 * 1024);
    Bytes b(n);
    Require(host.read_romfs(host.userdata, path, 0, b.data(), n) == n);
    return b;
}
struct Pak {
    Bytes b;
    std::size_t files, folders, ft;
    explicit Pak(Bytes data) : b(std::move(data)) {
        Require(b.size() >= 40 && std::memcmp(b.data(), "GFLXPACK", 8) == 0);
        files = Read(b, 16, 4); folders = Read(b, 20, 4); ft = Read(b, 24, 8);
        Require(files <= 16384 && folders <= 4096);
        Require(ft <= b.size() && files * 24 <= b.size() - ft);
        Require(folders * 8 <= b.size() - 40);
    }
    Bytes Member(std::size_t i) const {
        Require(i < files);
        auto p = ft + i * 24;
        auto comp = Read(b, p+2, 2), n = Read(b, p+4, 4), c = Read(b, p+8, 4), off = Read(b, p+16, 8);
        Require(n && n <= 16 * 1024 * 1024 && off <= b.size() && c <= b.size() - off);
        if (comp == 0) { Require(n == c); return Bytes(b.begin()+off, b.begin()+off+c); }
        Require(comp == 2);
        Bytes out(n);
        Require(LZ4_decompress_safe(reinterpret_cast<const char*>(b.data()+off),
                                  reinterpret_cast<char*>(out.data()), int(c), int(n)) == int(n));
        return out;
    }
    Bytes Named(std::string_view name) const {
        for (std::size_t i=0; i<folders; ++i) {
            auto p = Read(b, 40+i*8, 8);
            Require(p <= b.size() && 16 <= b.size()-p);
            auto n = Read(b, p+8, 4);
            Require(n <= files && n*16 <= b.size()-p-16);
            for (std::size_t j=0; j<n; ++j)
                if (Read(b, p+16+j*16, 8) == Hash(name)) return Member(Read(b, p+24+j*16, 4));
        }
        throw std::runtime_error("encounter member absent");
    }
};
struct Table {
    const Bytes& b;
    std::size_t p;
    explicit Table(const Bytes& bytes) : Table(bytes, Read(bytes, 0, 4)) {}
    Table(const Bytes& bytes, std::size_t pos) : b(bytes), p(pos) {
        Read(b, p, 4);
        const auto vt = Vt();
        auto n = Read(b, vt, 2), obj = Read(b, vt+2, 2);
        Require(n >= 4 && !(n&1) && vt <= b.size() && n <= b.size()-vt);
        Require(obj >= 4 && p <= b.size() && obj <= b.size()-p);
    }
    std::size_t Vt() const {
        auto delta = static_cast<std::int32_t>(Read(b, p, 4));
        auto v = static_cast<std::int64_t>(p) - delta;
        Require(v >= 0 && static_cast<std::uint64_t>(v) < b.size());
        return static_cast<std::size_t>(v);
    }
    std::size_t Field(unsigned i, unsigned width) const {
        auto vt = Vt(), n = Read(b, vt, 2);
        if (4+i*2+2 > n) return 0;
        auto off = Read(b, vt+4+i*2, 2);
        if (!off) return 0;
        auto obj = Read(b, vt+2, 2);
        Require(off >= 4 && off <= obj && width <= obj-off);
        Read(b, p+off, width);
        return p+off;
    }
    std::uint64_t Scalar(unsigned i, unsigned width) const {
        auto f = Field(i, width); return f ? Read(b, f, width) : 0;
    }
    std::size_t Target(std::size_t f) const {
        Require(f != 0);
        auto rel = Read(b, f, 4); Require(rel && rel <= b.size()-f);
        return f+rel;
    }
    Table Child(unsigned i) const { return Table(b, Target(Field(i,4))); }
    std::vector<Table> Tables(unsigned i) const {
        auto f = Field(i,4);
        if (!f) return {};
        auto v = Target(f), n = Read(b,v,4);
        Require(n <= 65536 && v+4 <= b.size() && n*4 <= b.size()-v-4);
        std::vector<Table> out;
        for (std::size_t j=0; j<n; ++j) out.emplace_back(b,Target(v+4+j*4));
        return out;
    }
};
} // namespace sw_archive
