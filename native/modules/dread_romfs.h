// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Metroid Dread romfs access for the asset-free package: an asset by its romfs path, read from the
// user's own game files the way mercury_engine_data_structures' FileTreeEditor.get_raw_asset finds
// it -- the file entry of a .pkg whose header lists the asset id, else a loose romfs file.
//
//   .pkg header: u32 header_size, u32 data_section_size, u32 count, count x {u64 asset id,
//                u32 start, u32 end} (absolute offsets into the .pkg), then the files.
//   asset id   : CRC-64 of the romfs path (reflected CRC-64/ECMA, init all ones, no xorout --
//                the engine's CStrId hash, DreadNameHash in the module).
//
// Only ranged reads: a .pkg is up to 300 MB; one header read plus one read of the asset.
// Thread-safety: an Assets object is thread-compatible (no internal locking); the module uses one
// per thread or guards it with its own mutex.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_abi.h"

namespace dread_romfs {

// Byte source over the romfs (bare paths such as "packs/system/system.pkg"). out == nullptr
// returns the file size (0 = missing); otherwise reads up to `size` bytes at `offset` and returns
// the count read.
using ReadFn = std::function<size_t(const char* path, uint64_t offset, void* out, size_t size)>;

// host.read_romfs as a ReadFn (reads chunked to <= 32 MiB per host call).
ReadFn HostReader(const EdenDsmodHostApi& host);

// The engine's path / name hash (reflected CRC-64/ECMA, init ~0, no xorout).
constexpr uint64_t Crc64(std::string_view s) {
    uint64_t crc = ~uint64_t{0};
    for (const char ch : s) {
        crc ^= static_cast<uint8_t>(ch);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? uint64_t{0xC96C5795D7870F42} : 0);
    }
    return crc;
}

struct PkgEntry {
    uint32_t start{}, end{};
};

class Assets {
public:
    explicit Assets(ReadFn read) : read_(std::move(read)) {}

    // The asset's bytes: from the first of `pkg_candidates` whose header lists crc64(asset_path)
    // (MEDS order: pkgs before loose files), else the loose romfs file `asset_path`. False (with
    // *err) when absent.
    bool ReadAsset(std::string_view asset_path, std::span<const std::string> pkg_candidates,
                   std::vector<uint8_t>& out, std::string* err = nullptr);
    // A whole loose romfs file. False when missing.
    bool ReadFile(std::string_view path, std::vector<uint8_t>& out);

    const ReadFn& Reader() const {
        return read_;
    }

private:
    struct PkgIndex {
        bool ok{};
        uint64_t size{};
        std::map<uint64_t, PkgEntry> entries; // a repeated id: the last entry wins (MEDS dict)
    };
    const PkgIndex& Index(const std::string& pkg);
    bool ReadRange(const std::string& path, uint64_t offset, size_t size, uint8_t* out);

    ReadFn read_;
    std::map<std::string, PkgIndex, std::less<>> pkgs_;
};

} // namespace dread_romfs
