// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dread_romfs.h"

#include <algorithm>
#include <cstring>

namespace dread_romfs {

ReadFn HostReader(const EdenDsmodHostApi& host) {
    const auto fn = host.read_romfs;
    void* const user = host.userdata;
    return [fn, user](const char* path, uint64_t offset, void* out, size_t size) -> size_t {
        if (!fn || !path)
            return 0;
        if (!out)
            return fn(user, path, 0, nullptr, 0);
        constexpr size_t Chunk = size_t(32) << 20;
        size_t done = 0;
        while (done < size) {
            const size_t want = std::min(Chunk, size - done);
            const size_t got =
                fn(user, path, offset + done, static_cast<uint8_t*>(out) + done, want);
            done += got;
            if (got != want)
                break;
        }
        return done;
    };
}

bool Assets::ReadRange(const std::string& path, uint64_t offset, size_t size, uint8_t* out) {
    return size == 0 || read_(path.c_str(), offset, out, size) == size;
}

const Assets::PkgIndex& Assets::Index(const std::string& pkg) {
    if (const auto it = pkgs_.find(pkg); it != pkgs_.end())
        return it->second;
    PkgIndex& index = pkgs_[pkg];
    const size_t size = read_ ? read_(pkg.c_str(), 0, nullptr, 0) : 0;
    uint8_t head[12];
    if (size < sizeof(head) || !ReadRange(pkg, 0, sizeof(head), head))
        return index;
    uint32_t count{};
    std::memcpy(&count, head + 8, 4);
    if (count > (size - 12) / 16)
        return index;
    std::vector<uint8_t> table(size_t(count) * 16);
    if (!ReadRange(pkg, 12, table.size(), table.data()))
        return index;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = table.data() + size_t(i) * 16;
        uint64_t id{};
        PkgEntry entry;
        std::memcpy(&id, e, 8);
        std::memcpy(&entry.start, e + 8, 4);
        std::memcpy(&entry.end, e + 12, 4);
        if (entry.end < entry.start || entry.end > size)
            continue;
        index.entries.insert_or_assign(id, entry);
    }
    index.size = size;
    index.ok = true;
    return index;
}

bool Assets::ReadFile(std::string_view path, std::vector<uint8_t>& out) {
    const std::string p{path};
    const size_t size = read_ ? read_(p.c_str(), 0, nullptr, 0) : 0;
    if (size == 0)
        return false;
    out.resize(size);
    return ReadRange(p, 0, size, out.data());
}

bool Assets::ReadAsset(std::string_view asset_path, std::span<const std::string> pkg_candidates,
                       std::vector<uint8_t>& out, std::string* err) {
    const uint64_t id = Crc64(asset_path);
    for (const auto& pkg : pkg_candidates) {
        const PkgIndex& index = Index(pkg);
        const auto it = index.entries.find(id);
        if (it == index.entries.end())
            continue;
        out.resize(it->second.end - it->second.start);
        if (ReadRange(pkg, it->second.start, out.size(), out.data()))
            return true;
        if (err)
            *err = "short read of " + std::string{asset_path} + " in " + pkg;
        return false;
    }
    if (ReadFile(asset_path, out))
        return true;
    if (err)
        *err = "asset not found: " + std::string{asset_path};
    return false;
}

} // namespace dread_romfs
