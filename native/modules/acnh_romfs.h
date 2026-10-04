// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The player's own ACNH romfs: raw reads (host read_romfs, or a dump directory in tests), zstd,
// SARC, and the two archive shapes the companion reads all its art from:
//   Layout/<Name>.Nin_NX_NVN.zs        zstd SARC -> <Name>.arc SARC -> blyt/*.bflyt, anim/*.bflan,
//                                      timg/__Combined.bntx (members flattened, "<arc>/<member>")
//   Model/Layout_<Fam>_<Name>.Nin_NX_NVN.zs   zstd SARC -> output.bfres (embedded BNTX)
// Decompressed archives are cached (LRU by bytes); everything is thread-safe.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_abi.h"

namespace acnh {

class Layout;

/// Members of a (possibly nested) SARC, flattened: nested SARC members are "<outer>/<inner>".
struct Archive {
    std::map<std::string, std::vector<uint8_t>, std::less<>> members;
    size_t bytes = 0;
    /// The first member whose name ends with `suffix` (e.g. ".bflyt", "__Combined.bntx").
    const std::vector<uint8_t>* FindSuffix(std::string_view suffix) const;
    /// Member by its last path component ("LMenuDeviceBtn_Type.bflan").
    const std::vector<uint8_t>* FindBase(std::string_view base) const;
};

class Romfs {
public:
    /// Reads through host->read_romfs (pointer + userdata copied; valid for the instance).
    explicit Romfs(const EdenDsmodHostApi* host);
    /// Reads files below a romfs dump directory (tests, tools).
    explicit Romfs(std::string directory);
    ~Romfs();
    Romfs(const Romfs&) = delete;
    Romfs& operator=(const Romfs&) = delete;

    /// `path` relative to the romfs root ("Layout/X.Nin_NX_NVN.zs"; a leading '/' or "romfs:" is
    /// accepted). False when missing, unreadable or larger than MaxFile.
    bool Read(std::string_view path, std::vector<uint8_t>& out);
    /// Read + zstd (a file that is not a zstd frame is returned as is).
    bool ReadZs(std::string_view path, std::vector<uint8_t>& out);
    /// A readable file of at most MaxFile bytes (its size only; nothing is read).
    bool Exists(std::string_view path);

    static bool Zstd(std::span<const uint8_t> in, std::vector<uint8_t>& out);
    static bool SarcFind(const std::vector<uint8_t>& sarc, std::string_view name,
                         std::vector<uint8_t>& out);
    static bool SarcList(const std::vector<uint8_t>& sarc, std::vector<std::string>& names);
    /// Every member of a SARC (zstd members unwrapped, nested SARCs flattened).
    /// Rejects malformed members, more than MaxFile expanded bytes or 32 nesting
    /// levels. Replaces out; failure clears it so partial archives cannot escape.
    static bool SarcAll(std::span<const uint8_t> sarc, Archive& out,
                        const std::string& prefix = {});

    /// Cached, decompressed archives (null when missing or malformed).
    std::shared_ptr<const Archive> Layout(std::string_view name);   ///< Layout/<name>.Nin_NX_NVN.zs
    std::shared_ptr<const Archive> Model(std::string_view name);    ///< Model/<name>.Nin_NX_NVN.zs
    std::shared_ptr<const Archive> SarcFile(std::string_view path); ///< any *.sarc(.zs) path

    /// Parsed layouts and decoded textures belong to this ROMFS instance, never a
    /// process-global cache shared across game/module lifetimes.
    std::shared_ptr<acnh::Layout> CachedLayout(std::string_view name);

    static constexpr size_t MaxFile = 64u << 20;

private:
    std::shared_ptr<const Archive> Cached(const std::string& path);

    std::function<bool(const std::string&, std::vector<uint8_t>*)> reader; ///< null out = exists
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<const Archive>> cache;
    std::map<std::string, std::shared_ptr<acnh::Layout>> layouts;
    std::vector<std::string> layout_lru;
    std::vector<std::string> lru; ///< most recent last
    size_t cached_bytes = 0;
    static constexpr size_t CacheBudget = 40u << 20;
};

} // namespace acnh
