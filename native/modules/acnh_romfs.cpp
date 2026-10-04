// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Romfs access, zstd and SARC for the ACNH companion. The SARC walker follows
// wonder_assets.cpp (copied, not shared, so the companions change independently).

#include "acnh_romfs.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include "acnh_bytes.h"
#include "acnh_lyt.h"
#include "core/mods/modules/dsmod_module_sdk.h"

namespace acnh {
namespace {
using namespace dsmod_sdk::int_types;

bool Magic(std::span<const u8> s, std::string_view m) {
    return s.size() >= m.size() && std::memcmp(s.data(), m.data(), m.size()) == 0;
}
template <typename F>
bool SarcWalk(std::span<const u8> s, F&& visit) {
    if (s.size() < 0x20 || !Magic(s, "SARC"))
        return false;
    if (!((s[6] == 0xfe && s[7] == 0xff) || (s[6] == 0xff && s[7] == 0xfe)))
        return false;
    const bool be = s[6] == 0xfe && s[7] == 0xff;
    const u32 hs = bytes::U16(s, 4, be);
    const u32 data = bytes::U32(s, 0xc, be);
    if (hs < 0x14 || hs + 12 > s.size() || data > s.size() || !Magic(s.subspan(hs), "SFAT") ||
        bytes::U16(s, hs + 4, be) != 12)
        return false;
    const u32 count = bytes::U16(s, hs + 6, be);
    const u32 nodes = hs + 12;
    if (count > 65535 || u64{nodes} + u64{count} * 16 > s.size())
        return false;
    const u32 sfnt = nodes + count * 16;
    if (sfnt + 8 > s.size() || !Magic(s.subspan(sfnt), "SFNT"))
        return false;
    const u32 name_header = bytes::U16(s, sfnt + 4, be);
    const u32 names = sfnt + name_header;
    if (name_header < 8 || names > data)
        return false;
    for (u32 i = 0; i < count; ++i) {
        const u32 n = nodes + i * 16;
        const u32 attr = bytes::U32(s, n + 4, be);
        const u32 begin = bytes::U32(s, n + 8, be), end = bytes::U32(s, n + 12, be);
        if (!(attr & 0x01000000))
            continue;
        const u64 no = names + u64{attr & 0xffffff} * 4;
        if (no >= data || begin > end || u64{data} + end > s.size())
            return false;
        const auto names_end = s.begin() + data;
        const auto zero = std::find(s.begin() + static_cast<ptrdiff_t>(no), names_end, 0);
        if (zero == names_end)
            return false;
        const std::string_view name(reinterpret_cast<const char*>(s.data() + no),
                                    static_cast<size_t>(zero - s.begin()) - no);
        if (visit(name, s.subspan(data + begin, end - begin)))
            return true;
    }
    return true;
}

std::string Normalize(std::string_view path) {
    if (path.starts_with("romfs:"))
        path.remove_prefix(6);
    while (path.starts_with("/"))
        path.remove_prefix(1);
    return std::string{path};
}

bool SafePath(std::string_view p) {
    if (p.empty() || p.size() > 512 || p.find("..") != std::string_view::npos)
        return false;
    return std::all_of(p.begin(), p.end(), [](unsigned char c) { return c >= 0x20 && c < 0x7f; });
}
} // namespace

const std::vector<uint8_t>* Archive::FindSuffix(std::string_view suffix) const {
    for (const auto& [name, bytes] : members)
        if (name.ends_with(suffix))
            return &bytes;
    return nullptr;
}

const std::vector<uint8_t>* Archive::FindBase(std::string_view base) const {
    for (const auto& [name, bytes] : members) {
        const auto slash = name.rfind('/');
        const std::string_view b = slash == std::string::npos
                                       ? std::string_view{name}
                                       : std::string_view{name}.substr(slash + 1);
        if (b == base)
            return &bytes;
    }
    return nullptr;
}

Romfs::Romfs(const EdenDsmodHostApi* host) {
    if (!host || !host->read_romfs)
        return;
    // `out` null = existence only (the host returns the size without reading)
    reader = [read_romfs = host->read_romfs, userdata = host->userdata](const std::string& path,
                                                                        std::vector<u8>* out) {
        const std::string p = "/" + path;
        const size_t size = read_romfs(userdata, p.c_str(), 0, nullptr, 0);
        if (!size || size > MaxFile)
            return false;
        if (!out)
            return true;
        out->resize(size);
        if (read_romfs(userdata, p.c_str(), 0, out->data(), size) != size) {
            out->clear();
            return false;
        }
        return true;
    };
}

Romfs::Romfs(std::string directory) {
    reader = [dir = std::move(directory)](const std::string& path, std::vector<u8>* out) {
        std::ifstream f(dir + "/" + path, std::ios::binary | std::ios::ate);
        if (!f)
            return false;
        const auto size = static_cast<size_t>(f.tellg());
        if (!size || size > MaxFile)
            return false;
        if (!out)
            return true;
        out->resize(size);
        f.seekg(0);
        if (!f.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(size))) {
            out->clear();
            return false;
        }
        return true;
    };
}

Romfs::~Romfs() = default;

bool Romfs::Read(std::string_view path, std::vector<uint8_t>& out) {
    out.clear();
    const std::string p = Normalize(path);
    if (!reader || !SafePath(p))
        return false;
    return reader(p, &out);
}

bool Romfs::Exists(std::string_view path) {
    const std::string p = Normalize(path);
    return reader && SafePath(p) && reader(p, nullptr); // the size only, nothing read
}

bool Romfs::ReadZs(std::string_view path, std::vector<uint8_t>& out) {
    out.clear();
    std::vector<u8> raw;
    if (!Read(path, raw))
        return false;
    return Zstd(raw, out);
}

bool Romfs::Zstd(std::span<const uint8_t> in, std::vector<uint8_t>& out) {
    out.clear();
    if (in.size() > MaxFile)
        return false;
    if (in.size() < 4 || dsmod_sdk::Le32(in.data()) != 0xFD2FB528u) {
        out.assign(in.begin(), in.end());
        return true;
    }
    const unsigned long long size = ZSTD_getFrameContentSize(in.data(), in.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size == 0 ||
        size > MaxFile)
        return false;
    out.resize(static_cast<size_t>(size));
    const size_t got = ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
    if (ZSTD_isError(got) || got != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

bool Romfs::SarcFind(const std::vector<uint8_t>& sarc, std::string_view name,
                     std::vector<uint8_t>& out) {
    out.clear();
    bool found = false;
    SarcWalk(sarc, [&](std::string_view n, std::span<const u8> bytes) {
        if (n != name)
            return false;
        out.assign(bytes.begin(), bytes.end());
        found = true;
        return true;
    });
    return found;
}

bool Romfs::SarcList(const std::vector<uint8_t>& sarc, std::vector<std::string>& names) {
    return SarcWalk(sarc, [&](std::string_view n, std::span<const u8>) {
        names.emplace_back(n);
        return false;
    });
}

namespace {
bool FlattenSarc(std::span<const uint8_t> sarc, Archive& out, const std::string& prefix,
                 unsigned depth, size_t& expanded) {
    // Each member is bounded individually by Zstd; also bound the entire expanded
    // archive and nesting so malformed archives cannot exhaust memory or the stack.
    if (depth >= 32)
        return false;
    bool valid = true;
    const bool walked = SarcWalk(sarc, [&](std::string_view n, std::span<const u8> bytes) {
        std::vector<u8> plain;
        if (!Romfs::Zstd(bytes, plain) || plain.size() > Romfs::MaxFile - expanded) {
            valid = false;
            return true;
        }
        expanded += plain.size(); // include intermediate nested archives in the budget
        if (prefix.size() > 512 || n.size() > 512 - prefix.size()) {
            valid = false;
            return true;
        }
        const std::string name = prefix + std::string{n};
        if (Magic(plain, "SARC")) {
            valid = FlattenSarc(plain, out, name + "/", depth + 1, expanded);
        } else if (plain.size() > Romfs::MaxFile - out.bytes) {
            valid = false;
        } else {
            const size_t size = plain.size();
            const auto [it, inserted] = out.members.emplace(name, std::move(plain));
            valid = inserted; // duplicate paths are ambiguous, not extra cache bytes
            if (inserted)
                out.bytes += size;
        }
        return !valid;
    });
    return walked && valid;
}
} // namespace

bool Romfs::SarcAll(std::span<const uint8_t> sarc, Archive& out, const std::string& prefix) {
    Archive parsed;
    size_t expanded = 0;
    if (!FlattenSarc(sarc, parsed, prefix, 0, expanded)) {
        out = {};
        return false;
    }
    out = std::move(parsed);
    return true;
}

std::shared_ptr<const Archive> Romfs::Cached(const std::string& path) {
    {
        std::scoped_lock lock{mutex};
        if (const auto it = cache.find(path); it != cache.end()) {
            lru.erase(std::find(lru.begin(), lru.end(), path));
            lru.push_back(path);
            return it->second;
        }
    }
    std::vector<u8> plain;
    std::shared_ptr<Archive> arc;
    if (ReadZs(path, plain)) {
        arc = std::make_shared<Archive>();
        if (!SarcAll(plain, *arc) || arc->members.empty())
            arc.reset();
    }
    if (!arc)
        return nullptr; // not cached: the host's romfs may simply not be open yet
    std::scoped_lock lock{mutex};
    if (const auto it = cache.find(path); it != cache.end())
        return it->second;
    cache[path] = arc;
    lru.push_back(path);
    cached_bytes += arc ? arc->bytes : 0;
    while (cached_bytes > CacheBudget && lru.size() > 1) {
        const std::string old = lru.front();
        lru.erase(lru.begin());
        if (const auto it = cache.find(old); it != cache.end()) {
            cached_bytes -= it->second ? it->second->bytes : 0;
            cache.erase(it);
        }
    }
    return arc;
}

std::shared_ptr<acnh::Layout> Romfs::CachedLayout(std::string_view name) {
    const std::string key{name};
    {
        std::scoped_lock lock{mutex};
        if (const auto it = layouts.find(key); it != layouts.end()) {
            layout_lru.erase(std::find(layout_lru.begin(), layout_lru.end(), key));
            layout_lru.push_back(key);
            return it->second;
        }
    }
    auto layout = acnh::Layout::Load(*this, name);
    if (!layout)
        return nullptr;
    std::scoped_lock lock{mutex};
    if (const auto it = layouts.find(key); it != layouts.end())
        return it->second;
    layouts.emplace(key, layout);
    layout_lru.push_back(key);
    if (layout_lru.size() > 16) {
        layouts.erase(layout_lru.front());
        layout_lru.erase(layout_lru.begin());
    }
    return layout;
}

std::shared_ptr<const Archive> Romfs::Layout(std::string_view name) {
    return Cached("Layout/" + std::string{name} + ".Nin_NX_NVN.zs");
}

std::shared_ptr<const Archive> Romfs::Model(std::string_view name) {
    return Cached("Model/" + std::string{name} + ".Nin_NX_NVN.zs");
}

std::shared_ptr<const Archive> Romfs::SarcFile(std::string_view path) {
    return Cached(Normalize(path));
}

} // namespace acnh
