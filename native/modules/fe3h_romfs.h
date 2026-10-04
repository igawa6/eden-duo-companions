// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fire Emblem: Three Houses (010055D009F78000) romfs access for the asset-free package: the
// game's LINKDATA archive, read from the player's own romfs through host.read_romfs in ranges.
// Layouts (checked against the whole 1.2.0 romfs, research/fe3h/data/REPORT.md §1):
//
//   DATA0.bin   31160 x 0x20 {u64 offset in DATA1, u64 plain size, u64 stored size,
//               u64 compressed flag}; size 0 = empty slot.
//   DATA1.bin   6.9 GB of entry payloads (plain, or KT-gz when the flag is 1).
//   patchN/INFO2.bin  {u64 INFO0 count, u64 INFO1 count}      (only the newest patchN has it;
//   patchN/INFO0.bin  count x 0x120 {u64 id, u64 size, u64 stored, u64 flag, char path[0x100]}
//                     -- replaces DATA1 entry <id> with the loose file "rom:/patchM/...".
//   patchN/INFO1.bin  0x118 records {u64 size, u64 stored, u64 flag, char path[0x100]}: files
//                     with no DATA0 slot, addressed by the game as id = DATA0 count + index.
//   KT-gz       u32 chunk size (power of two), u32 chunk count n, u32 plain size,
//               u32 stored size[n]; chunk 0 at align(12 + 4n, 0x80); a chunk is either raw
//               (stored == its plain size) or {u32 zlib length, zlib stream}; the next chunk
//               starts at align(chunk start + stored, 0x80).
//   ptbl        u32 n, n x {u32 offset, u32 size} relative to the container.
//
// Everything is bounds-checked and fails closed (false / null). Only KT-gz chunks that cover a
// requested range are inflated, so one 64 KiB portrait out of a 9 MB texture pack costs one or
// two chunk reads. Thread-compatible (no internal locking): fe3h_assets::Library serialises.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"

namespace fe3h_romfs {

// Byte source over the romfs (bare paths such as "DATA1.bin"). out == nullptr returns the file
// size (0 = missing); otherwise reads up to `size` bytes at `offset` and returns the count read.
using ReadFn = std::function<size_t(const char* path, uint64_t offset, void* out, size_t size)>;

// host.read_romfs as a ReadFn (chunked to <= 32 MiB per host call; the host caps at 64 MiB).
ReadFn HostReader(const EdenDsmodHostApi& host);

// Largest plain buffer any single read of this module materialises.
inline constexpr uint64_t MaxBlob = uint64_t{64} << 20;

// Random-access plain bytes (a file range, a KT-gz container, a ptbl child ...).
class Source {
public:
    virtual ~Source() = default;
    virtual uint64_t Size() const = 0;
    // Exactly `size` bytes at `offset` into out; false when out of range or on any error.
    virtual bool Read(uint64_t offset, size_t size, uint8_t* out) = 0;
    bool ReadVec(uint64_t offset, size_t size, std::vector<uint8_t>& out);
    bool ReadAll(std::vector<uint8_t>& out, uint64_t max = MaxBlob);
};
using SourcePtr = std::shared_ptr<Source>;

// [offset, offset + size) of a romfs file.
SourcePtr FileRange(ReadFn read, std::string path, uint64_t offset, uint64_t size);
// [offset, offset + size) of another source.
SourcePtr SubRange(SourcePtr inner, uint64_t offset, uint64_t size);
// Owned bytes.
SourcePtr Memory(std::vector<uint8_t> bytes);
// The plain view of a KT-gz container stored in `inner`; null when the header does not validate.
SourcePtr KtGz(SourcePtr inner);

// Whole-buffer helpers (tests, small containers).
bool LooksKtGz(std::span<const uint8_t> d);
bool KtGzDecompress(std::span<const uint8_t> in, std::vector<uint8_t>& out, uint64_t max = MaxBlob);
// zlib stream (RFC 1950) -> bytes; checks the header and the Adler-32 trailer, and that exactly
// `expect` bytes come out (expect == 0: any size up to max).
bool ZlibInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t expect,
                 size_t max = MaxBlob);

// ptbl child `index` of `container` (a source that is a ptbl). count (optional) gets n.
SourcePtr PtblChild(const SourcePtr& container, uint32_t index, uint32_t* count = nullptr);
bool PtblCount(const SourcePtr& container, uint32_t& count);

struct EntryInfo {
    uint64_t offset{};  // in DATA1.bin (unused for patch files)
    uint64_t size{};    // plain size
    uint64_t stored{};  // bytes on disk
    bool compressed{};  // KT-gz
    std::string path;   // romfs path of the patch file ("patch4/..."), empty = DATA1
};

class LinkData {
public:
    explicit LinkData(ReadFn read) : read_(std::move(read)) {}

    // Reads DATA0.bin and the newest patchN/INFO0.bin. Idempotent; false if DATA0 is unusable.
    bool Open();
    bool IsOpen() const {
        return open_;
    }
    size_t Count() const {
        return entries_.size(); // DATA0 ids; INFO1 files follow as Count() + index
    }
    size_t ExtraCount() const {
        return extra_.size();
    }
    // Entry after the patch override. Null for an unknown / empty id.
    const EntryInfo* Info(uint32_t id) const;
    // The entry's plain bytes as a Source (KT-gz entries inflate on demand). Null on failure.
    SourcePtr Entry(uint32_t id);
    std::string PatchDir() const {
        return patch_dir_;
    }
    size_t PatchedCount() const {
        return patched_;
    }
    const ReadFn& Reader() const {
        return read_;
    }

private:
    ReadFn read_;
    bool open_{};
    bool tried_{};
    uint64_t data1_size_{};
    std::vector<EntryInfo> entries_;
    std::vector<EntryInfo> extra_; // INFO1 records: id = entries_.size() + index
    std::string patch_dir_;
    size_t patched_{};
};

} // namespace fe3h_romfs
