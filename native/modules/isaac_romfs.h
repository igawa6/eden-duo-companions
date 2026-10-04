// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Binding of Isaac: Repentance (010021C000B6A000 + DLC 010021C000B6B001): file lookup in the
// game's own mount order (RebuildContentMountPoints @Repentance.nro+0x3B3510, get_mount_point
// @+0x4C222C; the last-added mount is searched first):
//   rp_patch/resources.<lang> > rp_patch/resources > aoc:resources.<lang> > aoc:resources >
//   resources.<lang> > resources
// `rp_patch/` and `resources*/` are in the patched program romfs ("romfs:"), `aoc:` is the
// Repentance DLC data romfs and `base:` the unpatched program romfs (runtime sources).
// Paths are the game's lower-case romfs names ('/' separated, relative to resources/).

#include "core/mods/dsmod_module_abi.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace isaac_romfs {

/// Game languages in stringtable column order.
enum class Lang : int { En = 0, Jp, Kr, Zh, Ru, De, Es, Fr };
std::string_view LangSuffix(Lang lang); // "" for En, "jp", "kr", ...

/// Read `rel` (path under resources/, e.g. "items.xml" or "gfx/ui/minimap1.anm2") through the
/// game's mount order. Returns false when no layer has the file. Any thread.
bool ReadResource(const EdenDsmodHostApi* host, std::string_view rel, Lang lang,
                  std::vector<std::uint8_t>& out);

/// Read one file from an explicit source: "romfs:...", "aoc:...", "base:...", "file:...".
bool ReadRaw(const EdenDsmodHostApi* host, std::string_view path, std::vector<std::uint8_t>& out);

/// The source path ReadResource would read for `rel` ("aoc:resources/gfx/ui/minimap2.pcx"), or
/// "" when no layer has it. Any thread.
std::string Resolve(const EdenDsmodHostApi* host, std::string_view rel, Lang lang);

/// A thread-safe cache over ReadResource/ReadRaw: remembers which layer holds a file (including
/// "no layer") and keeps recently read files' bytes within a byte budget (LRU). Shared pointers
/// keep a file alive for a reader while the cache evicts it.
class FileCache {
public:
    explicit FileCache(std::size_t budget = std::size_t(16) << 20) : budget_{budget} {}
    using Bytes = std::shared_ptr<const std::vector<std::uint8_t>>;
    /// Mount-order lookup (see ReadResource). Null when missing.
    Bytes Resource(const EdenDsmodHostApi* host, std::string_view rel, Lang lang);
    /// Explicit source ("base:resources/wiiubottomscreen.pcx"). Null when missing.
    Bytes Raw(const EdenDsmodHostApi* host, std::string_view path);
    void Clear();

private:
    Bytes Get(const EdenDsmodHostApi* host, const std::string& key, std::string_view rel,
              Lang lang, bool raw);
    std::mutex mu_;
    std::size_t budget_, used_{};
    std::list<std::string> lru_;
    struct Entry {
        Bytes bytes;
        std::list<std::string>::iterator it;
    };
    std::unordered_map<std::string, Entry> files_;
    std::unordered_map<std::string, bool> missing_;
};

} // namespace isaac_romfs
