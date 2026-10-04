// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// P5R asset-free package, decoder layer.
//
// Everything here reads the user's own P5R romfs through host.read_romfs and decodes it in the
// module; nothing game-derived is compiled in. Formats (all byte-verified against a local
// unpack of ALL_USEU.CPK and Pillow, see p5r_romfs_assets_test.cpp):
//   * CPK        CRI container: "CPK " section + @UTF CpkHeader at 0x10; TOC rows give
//                DirName/FileName/FileSize/ExtractSize/FileOffset; FileOffset is relative to
//                TocOffset (NOT ContentOffset). ALL_USEU.CPK is 15 GB: only ranged reads.
//   * CRILAYLA   CRI backward LZ (+0x100-byte raw prefix) for entries with FileSize<ExtractSize.
//   * DDS        DXT1/DXT3/DXT5 (the game's used art is 100% DXT5), BC4/BC5, BC7 (DX10 98/99:
//                only EN/.. MYPALACE movie stills), uncompressed mask formats (RGBA32, L8A8).
//   * SPR0 (.SPD) multi-texture sprite sheets (a repeated sprite id: the last one wins).
//   * Atlus PAK (v1/v2/v3), FTD0/wTD0 tables, NAME.TBL, MYPTABLE.BIN, ACB @UTF (BGM cue -> wave).
// Thread-safety: a Romfs is immutable after Open(); Read()/caches may be used from any thread
// (the caches take an internal mutex). Every parser bounds-checks and fails closed.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_abi.h"

namespace p5r_assets {

// Straight (non-premultiplied) RGBA8, row-major, w*h*4 bytes.
struct Image {
    uint32_t w{}, h{};
    std::vector<uint8_t> rgba;
};

// Byte source over the romfs ("CPK/ALL_USEU.CPK" ...). out == nullptr returns the file size
// (0 = missing); otherwise reads up to `size` bytes at `offset` and returns the count read.
using ReadFn = std::function<size_t(const char* path, uint64_t offset, void* out, size_t size)>;
ReadFn HostReader(const EdenDsmodHostApi& host); // wraps host.read_romfs (chunked <= 32 MiB)

// ------------------------------------------------------------------------------------ @UTF
// CRI generic table. Owns a copy of the packet bytes (starting at "@UTF").
class UtfTable {
public:
    enum Type : uint8_t {
        U8 = 0,
        S8,
        U16,
        S16,
        U32,
        S32,
        U64,
        S64,
        Float,
        String = 0xA,
        Data = 0xB
    };
    bool Parse(std::span<const uint8_t> packet);
    bool Parse(std::vector<uint8_t>&& packet);
    std::string_view Name() const;
    uint32_t Rows() const {
        return rows_;
    }
    int Column(std::string_view name) const; // -1 when absent
    // Integers of any width (signed ones sign-extended); false for a non-integer column.
    bool Int(uint32_t row, int col, int64_t& v) const;
    bool Str(uint32_t row, int col, std::string_view& v) const;
    bool Bytes(uint32_t row, int col, std::span<const uint8_t>& v) const;
    // Convenience: by name, with fallback when the column/row is missing or has another type.
    int64_t IntOr(uint32_t row, std::string_view col, int64_t fallback) const;
    std::string_view StrOr(uint32_t row, std::string_view col,
                           std::string_view fallback = {}) const;
    std::span<const uint8_t> BytesOr(uint32_t row, std::string_view col) const;
    size_t MemoryBytes() const {
        return bytes_.capacity() + cols_.capacity() * sizeof(Col);
    }

private:
    struct Col {
        uint8_t flags{};
        uint32_t name{};  // string-table offset
        uint32_t value{}; // absolute packet offset of the constant (0x20) or row offset (0x40)
    };
    bool Value(uint32_t row, int col, size_t& pos, uint8_t& type) const;
    bool CStr(uint32_t off, std::string_view& out) const;
    std::vector<uint8_t> bytes_;
    std::vector<Col> cols_;
    uint32_t rows_{}, row_len_{}, rows_base_{}, strings_base_{}, data_base_{}, name_{};
};

// ------------------------------------------------------------------------------------ CPK
class Cpk {
public:
    struct Entry {
        uint64_t offset{};  // absolute offset in the .CPK
        uint64_t size{};    // stored size
        uint64_t extract{}; // final size (== size: raw, else CRILAYLA)
    };
    bool Open(const ReadFn& read, std::string cpk_path, std::string* error = nullptr);
    bool IsOpen() const {
        return !path_.empty();
    }
    // Case-insensitive "DIR/NAME" lookup ('\\' and leading '/' are tolerated). O(log n).
    bool Find(std::string_view path, Entry& out) const;
    bool ReadEntry(const Entry& e, std::vector<uint8_t>& out, std::string* error = nullptr) const;
    size_t Count() const {
        return order_.size();
    }
    bool At(size_t sorted_index, std::string& path, Entry& out) const; // sorted order
    const std::string& Path() const {
        return path_;
    }
    size_t MemoryBytes() const {
        return toc_.MemoryBytes() + order_.capacity() * 4;
    }

private:
    bool RowEntry(uint32_t row, Entry& out) const;
    int Compare(uint32_t row, std::string_view path) const;
    ReadFn read_;
    std::string path_;
    UtfTable toc_;
    uint64_t toc_base_{};
    int dir_{-1}, name_{-1}, size_{-1}, extract_{-1}, offset_{-1};
    std::vector<uint32_t> order_; // rows sorted by path
};

// CRILAYLA ("CRILAYLA" magic). expected_size = TOC ExtractSize (0 = don't check).
bool IsCrilayla(std::span<const uint8_t> data);
bool DecompressCrilayla(std::span<const uint8_t> data, std::vector<uint8_t>& out,
                        size_t expected_size = 0);

struct SpdFile; // parsed SPR0 + its bytes (below)

// ------------------------------------------------------------------------------------ Romfs
// The running title's romfs: CPK/PATCH1.CPK entries override CPK/ALL_USEU.CPK. PATCH1 stores
// its files under "PATCH1/<path>"; a lookup of <path> tries "PATCH1/<path>", then "<path>" in
// PATCH1, then ALL_USEU, then a loose romfs file of exactly that path.
class Romfs {
public:
    Romfs();
    ~Romfs();
    Romfs(const Romfs&) = delete;
    Romfs& operator=(const Romfs&) = delete;

    bool Open(const EdenDsmodHostApi& host); // parse TOCs once, cache the index
    bool Open(ReadFn read);                  // tests/tools: any byte source
    bool IsOpen() const {
        return base_.IsOpen();
    }
    // "EN/INIT/P5CAMP_00SPD.SPD" etc., CRILAYLA-decompressed. Never caches.
    bool Read(std::string_view path, std::vector<uint8_t>& out) const;
    bool Stat(std::string_view path, uint64_t& extract_size) const;
    const Cpk& Base() const {
        return base_;
    }
    const Cpk& Patch() const {
        return patch_;
    }
    double OpenMilliseconds() const {
        return open_ms_;
    }
    size_t IndexBytes() const {
        return base_.MemoryBytes() + patch_.MemoryBytes();
    }
    std::string Describe() const; // one line for logs
    const std::string& Error() const {
        return error_;
    }

    // Shared, LRU-cached decompressed files (SPD sheets, ICON.DDS ...). Budget in bytes of file
    // data (default 48 MiB); the most recent file is always kept even when it alone is larger.
    std::shared_ptr<const std::vector<uint8_t>> CachedFile(std::string_view path) const;
    std::shared_ptr<const SpdFile> CachedSpd(std::string_view path) const;
    void SetCacheBudget(size_t bytes);
    size_t CacheBytes() const;
    void ClearCache() const;

private:
    struct Cache;
    ReadFn read_;
    Cpk base_, patch_;
    double open_ms_{};
    std::string error_;
    std::unique_ptr<Cache> cache_;
};

// ------------------------------------------------------------------------------------ DDS
enum class DdsFormat : uint8_t { Unknown, BC1, BC2, BC3, BC4, BC5, BC7, Masked };
struct DdsInfo {
    uint32_t w{}, h{}, mips{};
    DdsFormat format{};
    size_t data{};              // offset of mip 0
    uint32_t bits{}, mask[4]{}; // Masked: bit count + R,G,B,A masks (L8A8: luminance in R mask)
    bool luminance{};
};
bool ParseDDS(std::span<const uint8_t> dds, DdsInfo& out);
bool DecodeDDS(std::span<const uint8_t> dds, Image& out); // all of mip 0
// Rectangle of mip 0 (Pillow crop semantics: texels outside the texture are (0,0,0,0)).
// Only the blocks the rectangle touches are decoded.
bool DecodeDDSRegion(std::span<const uint8_t> dds, int32_t x, int32_t y, uint32_t w, uint32_t h,
                     Image& out);

// ------------------------------------------------------------------------------------ SPR0
struct Sprite {
    uint32_t id, tex, x, y, w, h;
    std::string name; // raw Shift-JIS bytes, trimmed at NUL (as stored)
};
struct SprTexture {
    uint32_t id{};
    std::span<const uint8_t> dds; // view into the SPD bytes
    std::string name;             // ASCII, trimmed at NUL
};
bool ParseSPR0(std::span<const uint8_t> spd, std::vector<Sprite>& sprites,
               std::vector<SprTexture>& textures);
// Contract form: textures_dds[i] = texture with id i (empty span when absent).
bool ParseSPR0(std::span<const uint8_t> spd, std::vector<Sprite>& sprites,
               std::vector<std::span<const uint8_t>>& textures_dds);

struct SpdFile {
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    std::vector<Sprite> sprites;
    std::vector<SprTexture> textures;
    std::map<uint32_t, size_t> by_id;     // sprite id -> index (last wins)
    std::map<uint32_t, size_t> tex_by_id; // texture id -> index (last wins)
    const Sprite* Find(uint32_t id) const;
    const SprTexture* Texture(uint32_t id) const;
};
bool ParseSpdFile(std::shared_ptr<const std::vector<uint8_t>> bytes, SpdFile& out);

// Sprite rect cut from its texture (zero-size sprites fail).
bool SpriteImage(Romfs& romfs, std::string_view spd_path, uint32_t sprite_id, Image& out);
// Whole texture of an SPD (tex id as in the SPD's texture table).
bool SpdTextureImage(Romfs& romfs, std::string_view spd_path, uint32_t tex_id, Image& out);
// EN/FONT/ICON.DDS: 6 x 14 cells, pitch 126 x 45, content 122 x 41 at (3+126c, 3+45r).
bool IconCell(Romfs& romfs, int cell, Image& out);
// Any DDS file of the romfs (CHARATEX busts, CARDTEX, HEROTEX, RMAP ...), mip 0.
bool DdsImage(Romfs& romfs, std::string_view dds_path, Image& out);

// ------------------------------------------------------------------------------------ tables
struct PakEntry {
    std::string name;
    std::span<const uint8_t> data;
};
// Atlus PAK: v3 (BE count, 32-byte names, BE sizes; CMM.BIN, MYPTABLE.BIN, ROADMAP.TBL),
// v2 (LE count, 32-byte names), v1 (252-byte names, no count).
bool ParsePak(std::span<const uint8_t> pak, std::vector<PakEntry>& out);
std::span<const uint8_t> PakFind(const std::vector<PakEntry>& entries, std::string_view name);

// FTD0 / wTD0 ("*.ctd", "*.ftd", "*.mtd"): u16 type at +0xC, u16 count at +0xE (type 1 = string
// list; a type-0 file's +0xC is a u32 count whose high half is 0), u32 BE entry offsets at +0x10.
bool ParseFtd(std::span<const uint8_t> ftd, std::vector<std::span<const uint8_t>>& entries);
// Type-0 entry: {u32 ?, u32 size, u32 rows, u32 ?} + rows.
struct FtdTable {
    uint32_t rows{}, row_size{};
    std::span<const uint8_t> data;
    std::span<const uint8_t> Row(uint32_t i) const {
        return i < rows ? data.subspan(size_t(i) * row_size, row_size) : std::span<const uint8_t>{};
    }
};
bool ParseFtdTable(std::span<const uint8_t> entry, FtdTable& out);
// First table of a CTD/FTD inside a PAK (e.g. CMM.BIN "cmmFormat.ctd").
bool ReadPakTable(Romfs& romfs, std::string_view pak_path, std::string_view member, FtdTable& out,
                  std::vector<uint8_t>& storage);

// EN/BATTLE/TABLE/NAME.TBL: sections of {BE u32 size, u16 BE offsets}, {BE u32 size, strings},
// each block 16-aligned. Strings are raw Atlus-encoded bytes (DecodeAtlusText).
bool ParseNameTbl(std::span<const uint8_t> tbl, std::vector<std::vector<std::string>>& sections);
bool ReadNameTbl(Romfs& romfs, std::vector<std::vector<std::string>>& sections,
                 std::string_view path = "EN/BATTLE/TABLE/NAME.TBL");

// Atlus EN text: ASCII bytes + 2-byte glyphs (hi,lo >= 0x80; index = (hi-0x80)*128+lo-0x80,
// index < 95 = ASCII, else p5r_dialogue::EnGlyph). Unknown glyphs: false, or `replacement`
// substituted when non-empty.
bool DecodeAtlusText(std::span<const uint8_t> raw, std::string& out,
                     std::string_view replacement = {});

// EN/INIT/MYPTABLE.BIN: mypSoundDataTable (16-byte rows, u16 BE cue at +0) and
// mypSoundNameTable (64-byte rows, Atlus text). Row r = Thieves Den row r = sprite r of
// EN/MYPALACE/SOUND/MUSIC_TITLE_001.SPD.
struct MypSound {
    uint16_t cue{};
    std::string name; // decoded name-table text (rows >= 78 are dev placeholders in the data)
};
bool ReadMypSounds(Romfs& romfs, std::vector<MypSound>& rows,
                   std::string_view path = "EN/INIT/MYPTABLE.BIN");

// BASE/SOUND/BGM.ACB: cue id -> WaveformTable index, resolved like the game's player
// (Cue -> Sequence -> Track -> TrackEvent -> Synth -> [OutsideLink ->] Waveform). Cues that
// don't follow that exact shape are left out.
bool ReadAcbCueWaveforms(std::span<const uint8_t> acb, std::map<uint32_t, uint32_t>& out);

// Music-player titles built from the game files (the table the module embedded up to v0.9.2):
// rows 0..77 carry their name-table text; rows >= 78 hold dev placeholders in the data -> the
// title exists only as sprite art (MUSIC_TITLE_001.SPD sprite `row`), `art_only` = true, text
// empty. Rows whose sprite is empty are not titled. Untitled cues inherit the one title of the
// titled cues sharing their waveform (inherited = true); silent waveform 41 is skipped.
struct MusicTitle {
    uint16_t cue{};
    uint8_t row{};
    bool inherited{}, art_only{};
    std::string text;
};
bool BuildMusicTitles(Romfs& romfs, std::vector<MusicTitle>& out);

// ------------------------------------------------------------------------------------ debug
// Decoder-level image sources for load_image / imgdump (p5r_recipes.h builds on the API):
//   module:p5r_dec:sprite:<spd path>:<sprite id>
//   module:p5r_dec:tex:<spd path>:<tex id>
//   module:p5r_dec:dds:<dds path>
//   module:p5r_dec:icon:<cell>
//   module:p5r_dec:probe       (logs romfs/CPK facts + timings through host.log, 1x1 image)
bool IsDecoderKey(std::string_view key);
bool LoadDecoderImage(Romfs& romfs, const EdenDsmodHostApi& host, std::string_view key, Image& out);

} // namespace p5r_assets
