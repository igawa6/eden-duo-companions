// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// P5R asset-free package, decoder layer. See p5r_romfs_assets.h.
#include "p5r_romfs_assets.h"

#include "core/mods/modules/dsmod_module_sdk.h"
#include "p5r_dialogue_reader.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <numeric>
#include <set>
#include <unordered_map>

namespace p5r_assets {
namespace {

using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point a, Clock::time_point b = Clock::now()) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

bool Has(std::span<const uint8_t> d, size_t pos, size_t n) {
    return pos <= d.size() && n <= d.size() - pos;
}
using dsmod_sdk::Be16;
using dsmod_sdk::Be32;
using dsmod_sdk::Be64;
using dsmod_sdk::Le16;
using dsmod_sdk::Le32;

char Up(char c) {
    return c >= 'a' && c <= 'z' ? char(c - 32) : c == '\\' ? '/' : c;
}

// Upper-case, '/'-separated, no leading '/', no "." / ".." components.
std::string NormalizePath(std::string_view p) {
    std::string out;
    out.reserve(p.size());
    for (char c : p)
        out.push_back(Up(c));
    while (!out.empty() && out.front() == '/')
        out.erase(out.begin());
    return out;
}

constexpr size_t MaxFileBytes = size_t(512) << 20; // no single P5R romfs asset comes close
constexpr size_t MaxTocBytes = size_t(64) << 20;

} // namespace

// ================================================================================ host reader
ReadFn HostReader(const EdenDsmodHostApi& host) {
    auto* fn = host.read_romfs;
    void* userdata = host.userdata;
    if (!fn)
        return {};
    return [fn, userdata](const char* path, uint64_t offset, void* out, size_t size) -> size_t {
        if (!out)
            return fn(userdata, path, 0, nullptr, 0);
        size_t done = 0;
        while (done < size) {
            const size_t chunk = std::min<size_t>(size - done, size_t(32) << 20);
            const size_t got =
                fn(userdata, path, offset + done, static_cast<uint8_t*>(out) + done, chunk);
            done += std::min(got, chunk);
            if (got < chunk)
                break;
        }
        return done;
    };
}

// ================================================================================ @UTF
namespace {
int TypeWidth(uint8_t type) {
    switch (type) {
    case 0:
    case 1:
        return 1;
    case 2:
    case 3:
        return 2;
    case 4:
    case 5:
    case 8:
    case 0xA:
        return 4;
    case 6:
    case 7:
    case 0xB:
        return 8;
    default:
        return -1;
    }
}
} // namespace

bool UtfTable::Parse(std::span<const uint8_t> packet) {
    return Parse(std::vector<uint8_t>(packet.begin(), packet.end()));
}

bool UtfTable::Parse(std::vector<uint8_t>&& packet) {
    bytes_.clear();
    cols_.clear();
    rows_ = 0;
    const std::span<const uint8_t> d{packet};
    if (!Has(d, 0, 0x20) || std::memcmp(d.data(), "@UTF", 4) != 0)
        return false;
    const uint32_t table_size = Be32(&d[4]);
    if (!Has(d, 8, table_size) || table_size < 0x18)
        return false;
    packet.resize(size_t(8) + table_size);
    const size_t P = 8;
    const uint32_t rows_off = Be16(&d[P + 2]);
    const uint32_t strings_off = Be32(&d[P + 4]);
    const uint32_t data_off = Be32(&d[P + 8]);
    const uint32_t name_off = Be32(&d[P + 12]);
    const uint32_t ncol = Be16(&d[P + 16]);
    const uint32_t row_len = Be16(&d[P + 18]);
    const uint32_t nrows = Be32(&d[P + 20]);
    const size_t size = packet.size();
    if (P + rows_off > size || P + strings_off > size || P + data_off > size)
        return false;
    if (uint64_t(row_len) * nrows > size - (P + rows_off))
        return false;
    std::vector<Col> cols;
    cols.reserve(ncol);
    size_t pos = 0x20;
    uint32_t row_pos = 0;
    for (uint32_t i = 0; i < ncol; ++i) {
        if (pos >= size)
            return false;
        Col c;
        c.flags = packet[pos++];
        const int width = TypeWidth(c.flags & 0x0F);
        if (width < 0)
            return false;
        if (c.flags & 0x10) {
            if (pos + 4 > size)
                return false;
            c.name = Be32(&packet[pos]);
            pos += 4;
        }
        if (c.flags & 0x20) {
            if (pos + width > size)
                return false;
            c.value = uint32_t(pos);
            pos += width;
        } else if (c.flags & 0x40) {
            c.value = row_pos;
            row_pos += width;
        }
        cols.push_back(c);
    }
    if (row_pos > row_len)
        return false;
    bytes_ = std::move(packet);
    cols_ = std::move(cols);
    rows_ = nrows;
    row_len_ = row_len;
    rows_base_ = uint32_t(P + rows_off);
    strings_base_ = uint32_t(P + strings_off);
    data_base_ = uint32_t(P + data_off);
    name_ = name_off;
    return true;
}

bool UtfTable::CStr(uint32_t off, std::string_view& out) const {
    const size_t start = size_t(strings_base_) + off;
    if (start >= bytes_.size())
        return false;
    const auto* b = bytes_.data();
    const void* end = std::memchr(b + start, 0, bytes_.size() - start);
    if (!end)
        return false;
    out = std::string_view(reinterpret_cast<const char*>(b + start),
                           static_cast<const uint8_t*>(end) - (b + start));
    return true;
}

std::string_view UtfTable::Name() const {
    std::string_view s;
    return CStr(name_, s) ? s : std::string_view{};
}

int UtfTable::Column(std::string_view name) const {
    for (size_t i = 0; i < cols_.size(); ++i) {
        std::string_view s;
        if ((cols_[i].flags & 0x10) && CStr(cols_[i].name, s) && s == name)
            return int(i);
    }
    return -1;
}

// pos = SIZE_MAX: the column is all-zero (name-only storage).
bool UtfTable::Value(uint32_t row, int col, size_t& pos, uint8_t& type) const {
    if (col < 0 || size_t(col) >= cols_.size() || row >= rows_)
        return false;
    const Col& c = cols_[col];
    type = c.flags & 0x0F;
    const int width = TypeWidth(type);
    if (c.flags & 0x20)
        pos = c.value;
    else if (c.flags & 0x40)
        pos = size_t(rows_base_) + size_t(row) * row_len_ + c.value;
    else {
        pos = SIZE_MAX;
        return true;
    }
    return pos + width <= bytes_.size();
}

bool UtfTable::Int(uint32_t row, int col, int64_t& v) const {
    size_t pos;
    uint8_t type;
    if (!Value(row, col, pos, type) || type > 7)
        return false;
    if (pos == SIZE_MAX) {
        v = 0;
        return true;
    }
    const uint8_t* p = &bytes_[pos];
    switch (type) {
    case 0:
        v = p[0];
        break;
    case 1:
        v = int8_t(p[0]);
        break;
    case 2:
        v = Be16(p);
        break;
    case 3:
        v = int16_t(Be16(p));
        break;
    case 4:
        v = Be32(p);
        break;
    case 5:
        v = int32_t(Be32(p));
        break;
    default:
        v = int64_t(Be64(p));
        break;
    }
    return true;
}

bool UtfTable::Str(uint32_t row, int col, std::string_view& v) const {
    size_t pos;
    uint8_t type;
    if (!Value(row, col, pos, type) || type != String)
        return false;
    if (pos == SIZE_MAX) {
        v = {};
        return true;
    }
    return CStr(Be32(&bytes_[pos]), v);
}

bool UtfTable::Bytes(uint32_t row, int col, std::span<const uint8_t>& v) const {
    size_t pos;
    uint8_t type;
    if (!Value(row, col, pos, type) || type != Data)
        return false;
    if (pos == SIZE_MAX) {
        v = {};
        return true;
    }
    const size_t off = size_t(data_base_) + Be32(&bytes_[pos]);
    const uint32_t n = Be32(&bytes_[pos + 4]);
    if (!Has(bytes_, off, n))
        return false;
    v = std::span<const uint8_t>(bytes_).subspan(off, n);
    return true;
}

int64_t UtfTable::IntOr(uint32_t row, std::string_view col, int64_t fallback) const {
    int64_t v;
    return Int(row, Column(col), v) ? v : fallback;
}
std::string_view UtfTable::StrOr(uint32_t row, std::string_view col,
                                 std::string_view fallback) const {
    std::string_view v;
    return Str(row, Column(col), v) ? v : fallback;
}
std::span<const uint8_t> UtfTable::BytesOr(uint32_t row, std::string_view col) const {
    std::span<const uint8_t> v;
    return Bytes(row, Column(col), v) ? v : std::span<const uint8_t>{};
}

// ================================================================================ CPK
namespace {
// "<tag><flags><u64 size>@UTF..." at `offset`: reads and parses the @UTF packet at +0x10.
bool ReadSection(const ReadFn& read, const std::string& path, uint64_t offset, const char* tag,
                 UtfTable& out, std::string* error) {
    uint8_t head[0x18];
    if (read(path.c_str(), offset, head, sizeof(head)) != sizeof(head)) {
        if (error)
            *error = "short read at section " + std::string(tag);
        return false;
    }
    if (std::memcmp(head, tag, 4) != 0 || std::memcmp(head + 0x10, "@UTF", 4) != 0) {
        if (error)
            *error = "bad section tag " + std::string(tag);
        return false;
    }
    const uint32_t table_size = Be32(head + 0x14);
    if (table_size > MaxTocBytes) {
        if (error)
            *error = "section too large";
        return false;
    }
    std::vector<uint8_t> packet(size_t(8) + table_size);
    if (read(path.c_str(), offset + 0x10, packet.data(), packet.size()) != packet.size()) {
        if (error)
            *error = "short read in section " + std::string(tag);
        return false;
    }
    if (!out.Parse(std::move(packet))) {
        if (error)
            *error = "bad @UTF in section " + std::string(tag);
        return false;
    }
    return true;
}
} // namespace

bool Cpk::Open(const ReadFn& read, std::string cpk_path, std::string* error) {
    path_.clear();
    order_.clear();
    if (!read) {
        if (error)
            *error = "no reader";
        return false;
    }
    UtfTable header;
    if (!ReadSection(read, cpk_path, 0, "CPK ", header, error))
        return false;
    const uint64_t toc = uint64_t(header.IntOr(0, "TocOffset", 0));
    if (toc == 0) {
        if (error)
            *error = "ITOC-only CPK is not supported";
        return false;
    }
    UtfTable table;
    if (!ReadSection(read, cpk_path, toc, "TOC ", table, error))
        return false;
    const int dir = table.Column("DirName"), name = table.Column("FileName"),
              size = table.Column("FileSize"), extract = table.Column("ExtractSize"),
              offset = table.Column("FileOffset");
    if (name < 0 || size < 0 || offset < 0) {
        if (error)
            *error = "TOC lacks FileName/FileSize/FileOffset";
        return false;
    }
    toc_ = std::move(table);
    dir_ = dir;
    name_ = name;
    size_ = size;
    extract_ = extract;
    offset_ = offset;
    toc_base_ = toc;
    read_ = read;
    // Sort rows by normalized path once (stable: the first TOC row wins among duplicates).
    const uint32_t n = toc_.Rows();
    std::vector<std::string> keys(n);
    for (uint32_t r = 0; r < n; ++r) {
        std::string_view d, f;
        if (dir_ >= 0)
            toc_.Str(r, dir_, d);
        toc_.Str(r, name_, f);
        std::string k;
        k.reserve(d.size() + f.size() + 1);
        for (char c : d)
            k.push_back(Up(c));
        if (!d.empty())
            k.push_back('/');
        for (char c : f)
            k.push_back(Up(c));
        keys[r] = std::move(k);
    }
    order_.resize(n);
    std::iota(order_.begin(), order_.end(), 0u);
    std::stable_sort(order_.begin(), order_.end(),
                     [&](uint32_t a, uint32_t b) { return keys[a] < keys[b]; });
    path_ = std::move(cpk_path);
    return true;
}

int Cpk::Compare(uint32_t row, std::string_view path) const {
    std::string_view d, f;
    if (dir_ >= 0)
        toc_.Str(row, dir_, d);
    toc_.Str(row, name_, f);
    size_t i = 0;
    auto step = [&](char c) -> int {
        if (i >= path.size())
            return 1;
        const unsigned char a = static_cast<unsigned char>(Up(c)),
                            b = static_cast<unsigned char>(path[i++]);
        return a < b ? -1 : a > b ? 1 : 0;
    };
    for (char c : d)
        if (int r = step(c))
            return r;
    if (!d.empty())
        if (int r = step('/'))
            return r;
    for (char c : f)
        if (int r = step(c))
            return r;
    return i < path.size() ? -1 : 0;
}

bool Cpk::RowEntry(uint32_t row, Entry& out) const {
    int64_t off = 0, size = 0, extract = 0;
    if (!toc_.Int(row, offset_, off) || !toc_.Int(row, size_, size) || off < 0 || size < 0)
        return false;
    if (extract_ < 0 || !toc_.Int(row, extract_, extract))
        extract = size;
    out.offset = toc_base_ + uint64_t(off);
    out.size = uint64_t(size);
    out.extract = uint64_t(extract);
    return true;
}

bool Cpk::Find(std::string_view path, Entry& out) const {
    if (order_.empty())
        return false;
    const std::string key = NormalizePath(path);
    size_t lo = 0, hi = order_.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (Compare(order_[mid], key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == order_.size() || Compare(order_[lo], key) != 0)
        return false;
    return RowEntry(order_[lo], out);
}

bool Cpk::At(size_t index, std::string& path, Entry& out) const {
    if (index >= order_.size())
        return false;
    const uint32_t row = order_[index];
    std::string_view d, f;
    if (dir_ >= 0)
        toc_.Str(row, dir_, d);
    toc_.Str(row, name_, f);
    path = d.empty() ? std::string(f) : std::string(d) + "/" + std::string(f);
    return RowEntry(row, out);
}

bool Cpk::ReadEntry(const Entry& e, std::vector<uint8_t>& out, std::string* error) const {
    if (!read_ || e.size > MaxFileBytes || e.extract > MaxFileBytes) {
        if (error)
            *error = "entry too large";
        return false;
    }
    std::vector<uint8_t> raw(e.size);
    if (read_(path_.c_str(), e.offset, raw.data(), raw.size()) != raw.size()) {
        if (error)
            *error = "short read";
        return false;
    }
    if (e.size == e.extract) {
        out = std::move(raw);
        return true;
    }
    if (!DecompressCrilayla(raw, out, e.extract)) {
        if (error)
            *error = "CRILAYLA decode failed";
        return false;
    }
    return true;
}

// ================================================================================ CRILAYLA
bool IsCrilayla(std::span<const uint8_t> d) {
    return d.size() >= 0x10 && std::memcmp(d.data(), "CRILAYLA", 8) == 0;
}

bool DecompressCrilayla(std::span<const uint8_t> d, std::vector<uint8_t>& out,
                        size_t expected_size) {
    if (!IsCrilayla(d) || d.size() < 0x10 + 0x100)
        return false;
    const uint32_t body = Le32(&d[8]);
    const uint32_t header = Le32(&d[12]);
    const size_t prefix = size_t(0x10) + header;
    const size_t out_size = size_t(body) + 0x100;
    if (!Has(d, prefix, 0x100) || out_size > MaxFileBytes ||
        (expected_size && expected_size != out_size))
        return false;
    out.assign(out_size, 0);
    std::memcpy(out.data(), &d[prefix], 0x100);
    // Backward bit reader, MSB first, from the byte before the 0x100 raw tail.
    ptrdiff_t pos = ptrdiff_t(d.size()) - 0x100 - 1;
    uint32_t pool = 0;
    int left = 0;
    bool bad = false;
    auto bits = [&](int n) -> uint32_t {
        uint32_t v = 0;
        while (n > 0) {
            if (left == 0) {
                if (pos < 0) {
                    bad = true;
                    return 0;
                }
                pool = d[size_t(pos--)];
                left = 8;
            }
            const int take = std::min(left, n);
            v = (v << take) | ((pool >> (left - take)) & ((1u << take) - 1));
            left -= take;
            n -= take;
        }
        return v;
    };
    uint8_t* o = out.data();
    size_t produced = 0;
    const size_t last = out_size - 1;
    while (produced < body && !bad) {
        if (bits(1)) {
            const size_t distance = bits(13) + 3;
            size_t length = 3;
            static constexpr int Groups[] = {2, 3, 5};
            int gi = 0;
            for (;;) {
                const int g = gi < 3 ? Groups[gi++] : 8;
                const uint32_t v = bits(g);
                length += v;
                if (bad || v != (1u << g) - 1)
                    break;
            }
            length = std::min(length, size_t(body) - produced);
            const size_t dst = last - produced;
            if (dst + distance > last)
                return false;
            for (size_t i = 0; i < length; ++i)
                o[dst - i] = o[dst - i + distance];
            produced += length;
        } else {
            o[last - produced] = uint8_t(bits(8));
            ++produced;
        }
    }
    return !bad;
}

// ================================================================================ SPR0
bool ParseSPR0(std::span<const uint8_t> d, std::vector<Sprite>& sprites,
               std::vector<SprTexture>& textures) {
    sprites.clear();
    textures.clear();
    if (!Has(d, 0, 0x20))
        return false;
    const uint32_t tc = Le16(&d[0x14]), sc = Le16(&d[0x16]);
    const uint32_t to = Le32(&d[0x18]), so = Le32(&d[0x1C]);
    if (!Has(d, to, size_t(tc) * 0x30) || !Has(d, so, size_t(sc) * 0xA0))
        return false;
    auto text = [&](size_t at, size_t n) {
        const char* p = reinterpret_cast<const char*>(&d[at]);
        return std::string(p, strnlen(p, n));
    };
    textures.reserve(tc);
    for (uint32_t i = 0; i < tc; ++i) {
        const size_t b = to + size_t(i) * 0x30;
        SprTexture t;
        t.id = Le32(&d[b]);
        const uint32_t off = Le32(&d[b + 8]), size = Le32(&d[b + 12]);
        if (Has(d, off, size))
            t.dds = d.subspan(off, size);
        t.name = text(b + 0x20, 0x10);
        textures.push_back(std::move(t));
    }
    sprites.reserve(sc);
    for (uint32_t i = 0; i < sc; ++i) {
        const size_t b = so + size_t(i) * 0xA0;
        Sprite s{Le32(&d[b]),        Le32(&d[b + 4]),    Le32(&d[b + 0x20]),  Le32(&d[b + 0x24]),
                 Le32(&d[b + 0x28]), Le32(&d[b + 0x2C]), text(b + 0x70, 0x30)};
        sprites.push_back(std::move(s));
    }
    return true;
}

bool ParseSPR0(std::span<const uint8_t> spd, std::vector<Sprite>& sprites,
               std::vector<std::span<const uint8_t>>& textures_dds) {
    std::vector<SprTexture> t;
    textures_dds.clear();
    if (!ParseSPR0(spd, sprites, t))
        return false;
    for (const auto& x : t) {
        if (x.id > 4096)
            continue;
        if (textures_dds.size() <= x.id)
            textures_dds.resize(x.id + 1);
        textures_dds[x.id] = x.dds;
    }
    return true;
}

const Sprite* SpdFile::Find(uint32_t id) const {
    const auto it = by_id.find(id);
    return it == by_id.end() ? nullptr : &sprites[it->second];
}
const SprTexture* SpdFile::Texture(uint32_t id) const {
    const auto it = tex_by_id.find(id);
    return it == tex_by_id.end() ? nullptr : &textures[it->second];
}

bool ParseSpdFile(std::shared_ptr<const std::vector<uint8_t>> bytes, SpdFile& out) {
    if (!bytes || !ParseSPR0(*bytes, out.sprites, out.textures))
        return false;
    out.bytes = std::move(bytes);
    out.by_id.clear();
    out.tex_by_id.clear();
    for (size_t i = 0; i < out.sprites.size(); ++i)
        out.by_id[out.sprites[i].id] = i;
    for (size_t i = 0; i < out.textures.size(); ++i)
        out.tex_by_id[out.textures[i].id] = i;
    return true;
}

// ================================================================================ Romfs
struct Romfs::Cache {
    struct Item {
        std::string key;
        std::shared_ptr<const std::vector<uint8_t>> bytes;
        std::shared_ptr<const SpdFile> spd;
        size_t cost{};
    };
    mutable std::mutex mutex;
    size_t budget = size_t(48) << 20;
    size_t used = 0;
    std::list<Item> lru; // front = most recent
    std::unordered_map<std::string, std::list<Item>::iterator> index;

    void Trim() {
        while (used > budget && lru.size() > 1) {
            used -= lru.back().cost;
            index.erase(lru.back().key);
            lru.pop_back();
        }
    }
};

Romfs::Romfs() : cache_(std::make_unique<Cache>()) {}
Romfs::~Romfs() = default;

bool Romfs::Open(const EdenDsmodHostApi& host) {
    return Open(HostReader(host));
}

bool Romfs::Open(ReadFn read) {
    const auto start = Clock::now();
    error_.clear();
    base_ = Cpk{};
    patch_ = Cpk{};
    ClearCache();
    read_ = std::move(read);
    if (!read_) {
        error_ = "romfs reader unavailable";
        return false;
    }
    if (!base_.Open(read_, "CPK/ALL_USEU.CPK", &error_)) {
        error_ = "CPK/ALL_USEU.CPK: " + error_;
        return false;
    }
    std::string patch_error;
    patch_.Open(read_, "CPK/PATCH1.CPK", &patch_error); // optional
    open_ms_ = Ms(start);
    return true;
}

bool Romfs::Stat(std::string_view path, uint64_t& extract_size) const {
    const std::string key = NormalizePath(path);
    Cpk::Entry e;
    if ((patch_.IsOpen() && (patch_.Find("PATCH1/" + key, e) || patch_.Find(key, e))) ||
        base_.Find(key, e)) {
        extract_size = e.extract;
        return true;
    }
    if (!read_)
        return false;
    const size_t size = read_(std::string(path).c_str(), 0, nullptr, 0);
    extract_size = size;
    return size > 0;
}

bool Romfs::Read(std::string_view path, std::vector<uint8_t>& out) const {
    const std::string key = NormalizePath(path);
    Cpk::Entry e;
    if (patch_.IsOpen() && (patch_.Find("PATCH1/" + key, e) || patch_.Find(key, e)))
        return patch_.ReadEntry(e, out);
    if (base_.Find(key, e))
        return base_.ReadEntry(e, out);
    if (!read_ || key.empty() || key.find("..") != std::string::npos)
        return false;
    // Loose romfs file (not inside a CPK). The CPKs themselves are never read whole.
    const std::string loose{path};
    const size_t size = read_(loose.c_str(), 0, nullptr, 0);
    if (size == 0 || size > (size_t(64) << 20))
        return false;
    out.resize(size);
    return read_(loose.c_str(), 0, out.data(), size) == size;
}

std::string Romfs::Describe() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "romfs: ALL_USEU %zu files, PATCH1 %zu files, index %.1f MiB, open %.1f ms%s%s",
                  base_.Count(), patch_.Count(), IndexBytes() / 1048576.0, open_ms_,
                  error_.empty() ? "" : ", error: ", error_.c_str());
    return buf;
}

std::shared_ptr<const std::vector<uint8_t>> Romfs::CachedFile(std::string_view path) const {
    const std::string key = NormalizePath(path);
    {
        std::scoped_lock lock{cache_->mutex};
        if (auto it = cache_->index.find(key); it != cache_->index.end()) {
            cache_->lru.splice(cache_->lru.begin(), cache_->lru, it->second);
            return it->second->bytes;
        }
    }
    auto bytes = std::make_shared<std::vector<uint8_t>>();
    if (!Read(path, *bytes))
        return nullptr;
    std::scoped_lock lock{cache_->mutex};
    if (auto it = cache_->index.find(key); it != cache_->index.end())
        return it->second->bytes; // raced: keep the first copy
    cache_->lru.push_front({key, bytes, nullptr, bytes->size()});
    cache_->index[key] = cache_->lru.begin();
    cache_->used += bytes->size();
    cache_->Trim();
    return bytes;
}

std::shared_ptr<const SpdFile> Romfs::CachedSpd(std::string_view path) const {
    const std::string key = NormalizePath(path);
    {
        std::scoped_lock lock{cache_->mutex};
        if (auto it = cache_->index.find(key); it != cache_->index.end() && it->second->spd) {
            cache_->lru.splice(cache_->lru.begin(), cache_->lru, it->second);
            return it->second->spd;
        }
    }
    auto bytes = CachedFile(path);
    if (!bytes)
        return nullptr;
    auto spd = std::make_shared<SpdFile>();
    if (!ParseSpdFile(bytes, *spd))
        return nullptr;
    std::scoped_lock lock{cache_->mutex};
    if (auto it = cache_->index.find(key); it != cache_->index.end()) {
        if (!it->second->spd) {
            it->second->spd = spd;
            const size_t extra = spd->sprites.size() * (sizeof(Sprite) + 48);
            it->second->cost += extra;
            cache_->used += extra;
        }
        return it->second->spd;
    }
    return spd; // evicted meanwhile: still valid for this caller
}

void Romfs::SetCacheBudget(size_t bytes) {
    std::scoped_lock lock{cache_->mutex};
    cache_->budget = bytes;
    cache_->Trim();
}
size_t Romfs::CacheBytes() const {
    std::scoped_lock lock{cache_->mutex};
    return cache_->used;
}
void Romfs::ClearCache() const {
    std::scoped_lock lock{cache_->mutex};
    cache_->lru.clear();
    cache_->index.clear();
    cache_->used = 0;
}

// ================================================================================ DDS
namespace {
constexpr uint32_t FourCC(const char* s) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}

size_t BlockBytes(DdsFormat f) {
    return f == DdsFormat::BC1 || f == DdsFormat::BC4 ? 8 : 16;
}

using Px = uint8_t[16][4];

void Expand565(uint16_t c, int rgb[3]) {
    const int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    rgb[0] = r << 3 | r >> 2;
    rgb[1] = g << 2 | g >> 4;
    rgb[2] = b << 3 | b >> 2;
}

void DecodeBC1(const uint8_t* b, Px px, bool four_colour) {
    const uint16_t c0 = Le16(b), c1 = Le16(b + 2);
    int p[4][4];
    Expand565(c0, p[0]);
    Expand565(c1, p[1]);
    p[0][3] = p[1][3] = p[2][3] = p[3][3] = 255;
    if (c0 > c1 || four_colour) {
        for (int k = 0; k < 3; ++k) {
            p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
            p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
        }
    } else {
        for (int k = 0; k < 3; ++k) {
            p[2][k] = (p[0][k] + p[1][k]) / 2;
            p[3][k] = 0;
        }
        p[3][3] = 0;
    }
    const uint32_t idx = Le32(b + 4);
    for (int i = 0; i < 16; ++i) {
        const int* c = p[idx >> (2 * i) & 3];
        for (int k = 0; k < 4; ++k)
            px[i][k] = uint8_t(c[k]);
    }
}

// BC3 alpha / BC4 channel block -> px[i][ch].
void DecodeAlphaBlock(const uint8_t* b, Px px, int ch) {
    const int a0 = b[0], a1 = b[1];
    int a[8] = {a0, a1};
    if (a0 > a1) {
        for (int i = 1; i < 7; ++i)
            a[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (int i = 1; i < 5; ++i)
            a[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i)
        px[i][ch] = uint8_t(a[bits >> (3 * i) & 7]);
}

// ---- BC7 (DirectX spec)
constexpr uint8_t kPartition2[64][16] = {{0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1},
                                         {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1},
                                         {0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1},
                                         {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1},
                                         {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1},
                                         {0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1},
                                         {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1},
                                         {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1},
                                         {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1},
                                         {0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
                                         {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1},
                                         {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1},
                                         {0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
                                         {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1},
                                         {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
                                         {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1},
                                         {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1},
                                         {0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
                                         {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0},
                                         {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0},
                                         {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
                                         {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0},
                                         {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0},
                                         {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1},
                                         {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0},
                                         {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0},
                                         {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0},
                                         {0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0},
                                         {0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0},
                                         {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
                                         {0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0},
                                         {0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0},
                                         {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1},
                                         {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1},
                                         {0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0},
                                         {0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0},
                                         {0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0},
                                         {0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0},
                                         {0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1},
                                         {0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1},
                                         {0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0},
                                         {0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0},
                                         {0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0},
                                         {0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0},
                                         {0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0},
                                         {0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1},
                                         {0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1},
                                         {0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0},
                                         {0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0},
                                         {0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0},
                                         {0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0},
                                         {0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0},
                                         {0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1},
                                         {0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1},
                                         {0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0},
                                         {0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0},
                                         {0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1},
                                         {0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1},
                                         {0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1},
                                         {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1},
                                         {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1},
                                         {0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
                                         {0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0},
                                         {0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1}};
constexpr uint8_t kPartition3[64][16] = {{0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2},
                                         {0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1},
                                         {0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1},
                                         {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1},
                                         {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2},
                                         {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2},
                                         {0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1},
                                         {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1},
                                         {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2},
                                         {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2},
                                         {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2},
                                         {0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2},
                                         {0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2},
                                         {0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2},
                                         {0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2},
                                         {0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0},
                                         {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2},
                                         {0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0},
                                         {0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2},
                                         {0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1},
                                         {0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2},
                                         {0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1},
                                         {0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2},
                                         {0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0},
                                         {0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0},
                                         {0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2},
                                         {0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0},
                                         {0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1},
                                         {0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2},
                                         {0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2},
                                         {0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1},
                                         {0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1},
                                         {0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2},
                                         {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1},
                                         {0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2},
                                         {0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0},
                                         {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0},
                                         {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0},
                                         {0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0},
                                         {0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1},
                                         {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1},
                                         {0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2},
                                         {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1},
                                         {0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2},
                                         {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1},
                                         {0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1},
                                         {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1},
                                         {0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1},
                                         {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2},
                                         {0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1},
                                         {0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2},
                                         {0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2},
                                         {0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2},
                                         {0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2},
                                         {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2},
                                         {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2},
                                         {0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2},
                                         {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2},
                                         {0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2},
                                         {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2},
                                         {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1},
                                         {0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2},
                                         {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
                                         {0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0}};
constexpr uint8_t kAnchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                                  15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
                                  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,
                                  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};
constexpr uint8_t kAnchor3a[64] = {3, 3,  15, 15, 8, 3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,
                                   3, 3,  8,  15, 3, 3,  6,  10, 5,  8,  8,  6,  8,  5,  15, 15,
                                   8, 15, 3,  5,  6, 10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15,
                                   3, 15, 5,  5,  5, 8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3};
constexpr uint8_t kAnchor3b[64] = {15, 8, 8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,
                                   15, 8, 15, 3,  15, 8,  15, 8,  3,  15, 6,  10, 15, 15, 10, 8,
                                   15, 3, 15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,
                                   15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};
constexpr uint8_t kW2[4] = {0, 21, 43, 64};
constexpr uint8_t kW3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr uint8_t kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Bc7Mode {
    uint8_t ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2;
};
constexpr Bc7Mode kModes[8] = {{3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0},
                               {3, 6, 0, 0, 5, 0, 0, 0, 2, 0}, {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
                               {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
                               {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}};

struct BitIn {
    const uint8_t* b;
    unsigned pos = 0;
    uint32_t Get(unsigned n) {
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i, ++pos)
            v |= uint32_t(b[pos >> 3] >> (pos & 7) & 1) << i;
        return v;
    }
};

uint8_t Interp(int e0, int e1, int w) {
    return uint8_t(((64 - w) * e0 + w * e1 + 32) >> 6);
}
const uint8_t* Weights(int bits) {
    return bits == 2 ? kW2 : bits == 3 ? kW3 : kW4;
}

void DecodeBC7(const uint8_t* block, Px px) {
    int mode = 0;
    while (mode < 8 && !(block[0] >> mode & 1))
        ++mode;
    if (mode == 8) { // reserved mode: opaque black, like Pillow (never produced by encoders)
        for (int i = 0; i < 16; ++i) {
            px[i][0] = px[i][1] = px[i][2] = 0;
            px[i][3] = 255;
        }
        return;
    }
    const Bc7Mode& m = kModes[mode];
    BitIn in{block, unsigned(mode + 1)};
    const uint32_t part = in.Get(m.pb);
    const uint32_t rot = in.Get(m.rb);
    const uint32_t isb = in.Get(m.isb);
    const int ne = m.ns * 2;
    int ep[6][4] = {};
    for (int c = 0; c < 3; ++c)
        for (int e = 0; e < ne; ++e)
            ep[e][c] = int(in.Get(m.cb));
    for (int e = 0; e < ne; ++e)
        ep[e][3] = m.ab ? int(in.Get(m.ab)) : 255;
    int cbits = m.cb, abits = m.ab;
    if (m.epb) {
        for (int e = 0; e < ne; ++e) {
            const int p = int(in.Get(1));
            for (int c = 0; c < 3; ++c)
                ep[e][c] = ep[e][c] << 1 | p;
            if (m.ab)
                ep[e][3] = ep[e][3] << 1 | p;
        }
        ++cbits;
        if (m.ab)
            ++abits;
    } else if (m.spb) {
        for (int s = 0; s < m.ns; ++s) {
            const int p = int(in.Get(1));
            for (int e = 2 * s; e < 2 * s + 2; ++e)
                for (int c = 0; c < 3; ++c)
                    ep[e][c] = ep[e][c] << 1 | p;
        }
        ++cbits;
    }
    for (int e = 0; e < ne; ++e) {
        for (int c = 0; c < 3; ++c)
            ep[e][c] = ep[e][c] << (8 - cbits) | ep[e][c] >> (2 * cbits - 8);
        if (m.ab)
            ep[e][3] = ep[e][3] << (8 - abits) | ep[e][3] >> (2 * abits - 8);
    }
    auto subset = [&](int i) -> int {
        return m.ns == 2 ? kPartition2[part][i] : m.ns == 3 ? kPartition3[part][i] : 0;
    };
    auto anchor = [&](int i) -> bool {
        if (i == 0)
            return true;
        if (m.ns == 2)
            return i == kAnchor2[part];
        if (m.ns == 3)
            return i == kAnchor3a[part] || i == kAnchor3b[part];
        return false;
    };
    int idx1[16], idx2[16] = {};
    for (int i = 0; i < 16; ++i)
        idx1[i] = int(in.Get(m.ib - (anchor(i) ? 1 : 0)));
    if (m.ib2)
        for (int i = 0; i < 16; ++i)
            idx2[i] = int(in.Get(m.ib2 - (i == 0 ? 1 : 0)));
    for (int i = 0; i < 16; ++i) {
        const int s = subset(i);
        const int* e0 = ep[2 * s];
        const int* e1 = ep[2 * s + 1];
        int ci = idx1[i], cb = m.ib, ai = idx1[i], ab = m.ib;
        if (m.ib2) {
            if (isb) {
                ci = idx2[i];
                cb = m.ib2;
                ai = idx1[i];
                ab = m.ib;
            } else {
                ci = idx1[i];
                cb = m.ib;
                ai = idx2[i];
                ab = m.ib2;
            }
        }
        uint8_t out[4];
        for (int c = 0; c < 3; ++c)
            out[c] = Interp(e0[c], e1[c], Weights(cb)[ci]);
        out[3] = m.ab ? Interp(e0[3], e1[3], Weights(ab)[ai]) : 255;
        if (rot)
            std::swap(out[3], out[rot - 1]);
        std::memcpy(px[i], out, 4);
    }
}

void DecodeBlock(DdsFormat f, const uint8_t* b, Px px) {
    switch (f) {
    case DdsFormat::BC1:
        DecodeBC1(b, px, false);
        break;
    case DdsFormat::BC2:
        DecodeBC1(b + 8, px, true);
        for (int i = 0; i < 16; ++i) {
            const int a = b[i / 2] >> (4 * (i & 1)) & 15;
            px[i][3] = uint8_t(a | a << 4);
        }
        break;
    case DdsFormat::BC3:
        DecodeBC1(b + 8, px, true);
        DecodeAlphaBlock(b, px, 3);
        break;
    case DdsFormat::BC4:
        DecodeAlphaBlock(b, px, 0);
        for (int i = 0; i < 16; ++i) {
            px[i][1] = px[i][2] = px[i][0];
            px[i][3] = 255;
        }
        break;
    case DdsFormat::BC5:
        DecodeAlphaBlock(b, px, 0);
        DecodeAlphaBlock(b + 8, px, 1);
        for (int i = 0; i < 16; ++i) {
            px[i][2] = 0;
            px[i][3] = 255;
        }
        break;
    case DdsFormat::BC7:
        DecodeBC7(b, px);
        break;
    default:
        std::memset(px, 0, sizeof(Px));
    }
}

uint8_t MaskChannel(uint32_t v, uint32_t mask) {
    if (!mask)
        return 0;
    int shift = 0;
    while (!(mask >> shift & 1))
        ++shift;
    const uint32_t max = mask >> shift;
    const uint32_t x = (v & mask) >> shift;
    return max == 255 ? uint8_t(x) : uint8_t((x * 255 + max / 2) / max);
}
} // namespace

bool ParseDDS(std::span<const uint8_t> d, DdsInfo& out) {
    out = {};
    if (!Has(d, 0, 128) || std::memcmp(d.data(), "DDS ", 4) != 0 || Le32(&d[4]) != 124)
        return false;
    out.h = Le32(&d[12]);
    out.w = Le32(&d[16]);
    out.mips = Le32(&d[28]);
    const uint32_t pf = Le32(&d[80]);
    const uint32_t cc = Le32(&d[84]);
    out.data = 128;
    if (out.w == 0 || out.h == 0 || out.w > 16384 || out.h > 16384)
        return false;
    if (pf & 0x4) {
        if (cc == FourCC("DXT1"))
            out.format = DdsFormat::BC1;
        else if (cc == FourCC("DXT2") || cc == FourCC("DXT3"))
            out.format = DdsFormat::BC2;
        else if (cc == FourCC("DXT4") || cc == FourCC("DXT5"))
            out.format = DdsFormat::BC3;
        else if (cc == FourCC("ATI1") || cc == FourCC("BC4U"))
            out.format = DdsFormat::BC4;
        else if (cc == FourCC("ATI2") || cc == FourCC("BC5U"))
            out.format = DdsFormat::BC5;
        else if (cc == FourCC("DX10")) {
            if (!Has(d, 128, 20))
                return false;
            const uint32_t dxgi = Le32(&d[128]);
            out.data = 148;
            switch (dxgi) {
            case 70:
            case 71:
            case 72:
                out.format = DdsFormat::BC1;
                break;
            case 73:
            case 74:
            case 75:
                out.format = DdsFormat::BC2;
                break;
            case 76:
            case 77:
            case 78:
                out.format = DdsFormat::BC3;
                break;
            case 79:
            case 80:
                out.format = DdsFormat::BC4;
                break;
            case 82:
            case 83:
                out.format = DdsFormat::BC5;
                break;
            case 97:
            case 98:
            case 99:
                out.format = DdsFormat::BC7;
                break;
            case 27:
            case 28:
            case 29:
                out.format = DdsFormat::Masked;
                out.bits = 32;
                out.mask[0] = 0xff;
                out.mask[1] = 0xff00;
                out.mask[2] = 0xff0000;
                out.mask[3] = 0xff000000;
                break;
            case 87:
            case 88:
            case 90:
            case 91:
                out.format = DdsFormat::Masked;
                out.bits = 32;
                out.mask[0] = 0xff0000;
                out.mask[1] = 0xff00;
                out.mask[2] = 0xff;
                out.mask[3] = (dxgi == 87 || dxgi == 90 || dxgi == 91) ? 0xff000000 : 0;
                break;
            default:
                return false;
            }
        } else
            return false;
    } else if (pf & (0x40 | 0x20000 | 0x2)) {
        out.format = DdsFormat::Masked;
        out.bits = Le32(&d[88]);
        for (int i = 0; i < 4; ++i)
            out.mask[i] = Le32(&d[92 + 4 * i]);
        out.luminance = (pf & 0x20000) != 0;
        if (!(pf & 0x3))
            out.mask[3] = 0; // no alpha channel
        if (out.bits != 8 && out.bits != 16 && out.bits != 24 && out.bits != 32)
            return false;
    } else
        return false;
    size_t need;
    if (out.format == DdsFormat::Masked)
        need = size_t((uint64_t(out.w) * out.bits + 7) / 8) * out.h;
    else
        need = size_t((out.w + 3) / 4) * ((out.h + 3) / 4) * BlockBytes(out.format);
    return Has(d, out.data, need);
}

bool DecodeDDSRegion(std::span<const uint8_t> d, int32_t x, int32_t y, uint32_t w, uint32_t h,
                     Image& out) {
    DdsInfo info;
    if (!ParseDDS(d, info) || w == 0 || h == 0 || w > 16384 || h > 16384)
        return false;
    out.w = w;
    out.h = h;
    out.rgba.assign(size_t(w) * h * 4, 0);
    const int64_t x0 = std::max<int64_t>(x, 0), y0 = std::max<int64_t>(y, 0);
    const int64_t x1 = std::min<int64_t>(int64_t(x) + w, info.w),
                  y1 = std::min<int64_t>(int64_t(y) + h, info.h);
    if (x0 >= x1 || y0 >= y1)
        return true; // entirely outside: transparent
    const uint8_t* base = d.data() + info.data;
    if (info.format == DdsFormat::Masked) {
        const size_t bpp = info.bits / 8;
        const size_t pitch = (size_t(info.w) * info.bits + 7) / 8;
        for (int64_t py = y0; py < y1; ++py) {
            const uint8_t* row = base + size_t(py) * pitch;
            uint8_t* dst = &out.rgba[(size_t(py - y) * w + size_t(x0 - x)) * 4];
            for (int64_t px = x0; px < x1; ++px, dst += 4) {
                uint32_t v = 0;
                for (size_t k = 0; k < bpp; ++k)
                    v |= uint32_t(row[size_t(px) * bpp + k]) << (8 * k);
                const uint8_t r = MaskChannel(v, info.mask[0]);
                if (info.luminance) {
                    dst[0] = dst[1] = dst[2] = r;
                } else {
                    dst[0] = r;
                    dst[1] = MaskChannel(v, info.mask[1]);
                    dst[2] = MaskChannel(v, info.mask[2]);
                }
                dst[3] = info.mask[3] ? MaskChannel(v, info.mask[3]) : 255;
            }
        }
        return true;
    }
    const size_t bs = BlockBytes(info.format);
    const size_t bw = (info.w + 3) / 4;
    Px px;
    for (int64_t by = y0 / 4; by <= (y1 - 1) / 4; ++by) {
        for (int64_t bx = x0 / 4; bx <= (x1 - 1) / 4; ++bx) {
            DecodeBlock(info.format, base + (size_t(by) * bw + size_t(bx)) * bs, px);
            for (int j = 0; j < 4; ++j) {
                const int64_t py = by * 4 + j;
                if (py < y0 || py >= y1)
                    continue;
                for (int i = 0; i < 4; ++i) {
                    const int64_t pxx = bx * 4 + i;
                    if (pxx < x0 || pxx >= x1)
                        continue;
                    std::memcpy(&out.rgba[(size_t(py - y) * w + size_t(pxx - x)) * 4],
                                px[j * 4 + i], 4);
                }
            }
        }
    }
    return true;
}

bool DecodeDDS(std::span<const uint8_t> d, Image& out) {
    DdsInfo info;
    return ParseDDS(d, info) && DecodeDDSRegion(d, 0, 0, info.w, info.h, out);
}

// ================================================================================ images
bool SpriteImage(Romfs& romfs, std::string_view spd_path, uint32_t sprite_id, Image& out) {
    const auto spd = romfs.CachedSpd(spd_path);
    if (!spd)
        return false;
    const Sprite* s = spd->Find(sprite_id);
    if (!s || !s->w || !s->h)
        return false;
    const SprTexture* t = spd->Texture(s->tex);
    if (!t || t->dds.empty() || s->x > INT32_MAX || s->y > INT32_MAX)
        return false;
    return DecodeDDSRegion(t->dds, int32_t(s->x), int32_t(s->y), s->w, s->h, out);
}

bool SpdTextureImage(Romfs& romfs, std::string_view spd_path, uint32_t tex_id, Image& out) {
    const auto spd = romfs.CachedSpd(spd_path);
    const SprTexture* t = spd ? spd->Texture(tex_id) : nullptr;
    return t && !t->dds.empty() && DecodeDDS(t->dds, out);
}

bool IconCell(Romfs& romfs, int cell, Image& out) {
    constexpr int Cols = 6, Rows = 14;
    if (cell < 0 || cell >= Cols * Rows)
        return false;
    const auto dds = romfs.CachedFile("EN/FONT/ICON.DDS");
    if (!dds)
        return false;
    const int c = cell % Cols, r = cell / Cols;
    return DecodeDDSRegion(*dds, 3 + 126 * c, 3 + 45 * r, 122, 41, out);
}

bool DdsImage(Romfs& romfs, std::string_view dds_path, Image& out) {
    std::vector<uint8_t> bytes;
    return romfs.Read(dds_path, bytes) && DecodeDDS(bytes, out);
}

// ================================================================================ tables
bool ParsePak(std::span<const uint8_t> d, std::vector<PakEntry>& out) {
    out.clear();
    auto name_ok = [&](size_t at, size_t n) {
        if (!Has(d, at, n) || d[at] == 0)
            return false;
        for (size_t i = 0; i < n && d[at + i]; ++i)
            if (d[at + i] < 0x20 || d[at + i] > 0x7e)
                return false;
        return true;
    };
    auto name_of = [&](size_t at, size_t n) {
        const char* p = reinterpret_cast<const char*>(&d[at]);
        return std::string(p, strnlen(p, n));
    };
    // v2 / v3: count, then {name[32], size, data} packed.
    for (const bool big : {true, false}) {
        if (!Has(d, 0, 4))
            break;
        const uint32_t count = big ? Be32(&d[0]) : Le32(&d[0]);
        if (count == 0 || count > 100000)
            continue;
        std::vector<PakEntry> entries;
        size_t pos = 4;
        bool ok = true;
        for (uint32_t i = 0; i < count && ok; ++i) {
            if (!name_ok(pos, 32) || !Has(d, pos + 32, 4)) {
                ok = false;
                break;
            }
            const uint32_t size = big ? Be32(&d[pos + 32]) : Le32(&d[pos + 32]);
            if (!Has(d, pos + 36, size)) {
                ok = false;
                break;
            }
            entries.push_back({name_of(pos, 32), d.subspan(pos + 36, size)});
            pos += 36 + size_t(size);
        }
        if (ok) {
            out = std::move(entries);
            return true;
        }
    }
    // v1: {name[252], u32 LE size, data} aligned to 64 bytes, until a blank name / the end.
    size_t pos = 0;
    std::vector<PakEntry> entries;
    while (Has(d, pos, 256) && d[pos] != 0) {
        if (!name_ok(pos, 252))
            return false;
        const uint32_t size = Le32(&d[pos + 252]);
        if (!Has(d, pos + 256, size))
            return false;
        entries.push_back({name_of(pos, 252), d.subspan(pos + 256, size)});
        pos = (pos + 256 + size_t(size) + 63) & ~size_t(63);
    }
    if (entries.empty())
        return false;
    out = std::move(entries);
    return true;
}

std::span<const uint8_t> PakFind(const std::vector<PakEntry>& entries, std::string_view name) {
    for (const auto& e : entries) {
        if (e.name.size() != name.size())
            continue;
        bool eq = true;
        for (size_t i = 0; i < name.size() && eq; ++i)
            eq = Up(e.name[i]) == Up(name[i]);
        if (eq)
            return e.data;
    }
    return {};
}

bool ParseFtd(std::span<const uint8_t> d, std::vector<std::span<const uint8_t>>& entries) {
    entries.clear();
    if (!Has(d, 0, 0x10) ||
        (std::memcmp(&d[4], "FTD0", 4) != 0 && std::memcmp(&d[4], "wTD0", 4) != 0))
        return false;
    const size_t declared = Be32(&d[8]);
    const auto file = d.first(std::min(declared ? declared : d.size(), d.size()));
    const uint32_t count = Be16(&file[0xE]);
    if (!Has(file, 0x10, size_t(count) * 4))
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t off = Be32(&file[0x10 + 4 * i]);
        if (off >= file.size())
            return false;
        entries.push_back(file.subspan(off));
    }
    return true;
}

bool ParseFtdTable(std::span<const uint8_t> e, FtdTable& out) {
    out = {};
    if (!Has(e, 0, 0x10))
        return false;
    const uint32_t size = Be32(&e[4]), rows = Be32(&e[8]);
    if (!Has(e, 0x10, size) || rows == 0 || size % rows != 0)
        return false;
    out.rows = rows;
    out.row_size = size / rows;
    out.data = e.subspan(0x10, size);
    return true;
}

bool ReadPakTable(Romfs& romfs, std::string_view pak_path, std::string_view member, FtdTable& out,
                  std::vector<uint8_t>& storage) {
    std::vector<PakEntry> entries;
    std::vector<std::span<const uint8_t>> ftd;
    if (!romfs.Read(pak_path, storage) || !ParsePak(storage, entries))
        return false;
    const auto data = PakFind(entries, member);
    return !data.empty() && ParseFtd(data, ftd) && !ftd.empty() && ParseFtdTable(ftd[0], out);
}

bool ParseNameTbl(std::span<const uint8_t> d, std::vector<std::vector<std::string>>& sections) {
    sections.clear();
    auto align = [](size_t x) { return (x + 15) & ~size_t(15); };
    size_t o = 0;
    while (o < d.size()) {
        if (!Has(d, o, 4))
            return false;
        const uint32_t n = Be32(&d[o]);
        if (!Has(d, o + 4, n))
            return false;
        const auto offs = d.subspan(o + 4, n);
        o = align(o + 4 + n);
        if (!Has(d, o, 4))
            return false;
        const uint32_t m = Be32(&d[o]);
        if (!Has(d, o + 4, m))
            return false;
        const auto strs = d.subspan(o + 4, m);
        o = align(o + 4 + m);
        std::vector<std::string> names;
        names.reserve(n / 2);
        for (size_t i = 0; i + 1 < offs.size(); i += 2) {
            const uint16_t off = Be16(&offs[i]);
            if (off > strs.size())
                return false;
            size_t e = off;
            while (e < strs.size() && strs[e])
                ++e;
            names.emplace_back(reinterpret_cast<const char*>(strs.data() + off), e - off);
        }
        sections.push_back(std::move(names));
    }
    return true;
}

bool ReadNameTbl(Romfs& romfs, std::vector<std::vector<std::string>>& sections,
                 std::string_view path) {
    std::vector<uint8_t> bytes;
    return romfs.Read(path, bytes) && ParseNameTbl(bytes, sections);
}

bool DecodeAtlusText(std::span<const uint8_t> raw, std::string& out, std::string_view replacement) {
    out.clear();
    for (size_t p = 0; p < raw.size() && raw[p];) {
        const uint8_t c = raw[p];
        if (c < 0x80) {
            out.push_back(char(c));
            ++p;
            continue;
        }
        if (p + 1 >= raw.size())
            return false;
        std::string glyph;
        if (!p5r_dialogue::EnGlyph(uint16_t(c << 8 | raw[p + 1]), glyph)) {
            if (replacement.empty())
                return false;
            glyph = replacement;
        }
        out += glyph;
        p += 2;
    }
    return true;
}

bool ReadMypSounds(Romfs& romfs, std::vector<MypSound>& rows, std::string_view path) {
    rows.clear();
    std::vector<uint8_t> bytes;
    std::vector<PakEntry> entries;
    if (!romfs.Read(path, bytes) || !ParsePak(bytes, entries))
        return false;
    auto table = [&](std::string_view prefix, FtdTable& t) {
        for (const auto& e : entries) {
            if (e.name.rfind(prefix, 0) != 0)
                continue;
            std::vector<std::span<const uint8_t>> ftd;
            return ParseFtd(e.data, ftd) && !ftd.empty() && ParseFtdTable(ftd[0], t);
        }
        return false;
    };
    FtdTable data, names;
    if (!table("mypSoundDataTable", data) || !table("mypSoundNameTable", names) ||
        data.row_size < 2 || names.row_size == 0)
        return false;
    const uint32_t n = std::min(data.rows, names.rows);
    rows.reserve(n);
    for (uint32_t r = 0; r < n; ++r) {
        MypSound s;
        s.cue = Be16(data.Row(r).data());
        if (!DecodeAtlusText(names.Row(r), s.name, "\xEF\xBF\xBD"))
            return false;
        rows.push_back(std::move(s));
    }
    return true;
}

bool ReadAcbCueWaveforms(std::span<const uint8_t> acb, std::map<uint32_t, uint32_t>& out) {
    out.clear();
    UtfTable header;
    if (!header.Parse(acb) || header.Rows() == 0)
        return false;
    UtfTable cue, seq, track, event, synth, link;
    auto sub = [&](const char* name, UtfTable& t) { return t.Parse(header.BytesOr(0, name)); };
    if (!sub("CueTable", cue) || !sub("SequenceTable", seq) || !sub("TrackTable", track) ||
        !sub("TrackEventTable", event) || !sub("SynthTable", synth))
        return false;
    const bool has_link = sub("OutsideLinkTable", link);
    std::map<uint32_t, uint32_t> row_of; // cue id -> cue row
    for (uint32_t r = 0; r < cue.Rows(); ++r)
        row_of[uint32_t(cue.IntOr(r, "CueId", -1))] = r;
    std::function<bool(uint32_t, int, uint32_t&)> wave = [&](uint32_t id, int depth,
                                                             uint32_t& w) -> bool {
        const auto it = row_of.find(id);
        if (it == row_of.end())
            return false;
        const uint32_t r = it->second;
        if (cue.IntOr(r, "ReferenceType", -1) != 3)
            return false;
        const int64_t si = cue.IntOr(r, "ReferenceIndex", -1);
        if (si < 0 || si >= seq.Rows() || seq.IntOr(uint32_t(si), "NumTracks", 0) != 1)
            return false;
        const auto ti_bytes = seq.BytesOr(uint32_t(si), "TrackIndex");
        if (ti_bytes.size() < 2)
            return false;
        const uint32_t ti = Be16(ti_bytes.data());
        const int64_t ei = track.IntOr(ti, "EventIndex", -1);
        if (ti >= track.Rows() || ei < 0 || ei >= event.Rows())
            return false;
        const auto cmd = event.BytesOr(uint32_t(ei), "Command");
        if (cmd.size() < 7 || cmd[0] != 0x07 || cmd[1] != 0xd0 || cmd[2] != 0x04 || cmd[3] != 0 ||
            cmd[4] != 2)
            return false;
        const uint32_t syn = Be16(&cmd[5]);
        if (syn >= synth.Rows())
            return false;
        const auto ri = synth.BytesOr(syn, "ReferenceItems");
        if (ri.size() != 4)
            return false;
        const uint32_t type = Be16(&ri[0]), index = Be16(&ri[2]);
        if (type == 1) {
            w = index;
            return true;
        }
        if (type == 5 && depth == 0 && has_link && index < link.Rows())
            return wave(uint32_t(link.IntOr(index, "Id", -1)), 1, w);
        return false;
    };
    for (const auto& [id, r] : row_of) {
        uint32_t w;
        if (wave(id, 0, w))
            out[id] = w;
    }
    return !out.empty();
}

bool BuildMusicTitles(Romfs& romfs, std::vector<MusicTitle>& out) {
    out.clear();
    constexpr unsigned PlaceholderRows = 78; // rows >= 78: name table holds dev placeholders
    constexpr uint32_t SilentWaveform = 41;  // AWB 53 decodes to digital silence
    constexpr uint32_t MaxBgmCue = 60000;
    std::vector<MypSound> rows;
    std::vector<uint8_t> acb;
    std::map<uint32_t, uint32_t> waves;
    if (!ReadMypSounds(romfs, rows) || !romfs.Read("BASE/SOUND/BGM.ACB", acb) ||
        !ReadAcbCueWaveforms(acb, waves))
        return false;
    const auto spd = romfs.CachedSpd("EN/MYPALACE/SOUND/MUSIC_TITLE_001.SPD");
    if (!spd)
        return false;
    // A row is shown only when its title sprite has >= 8 px of alpha width.
    auto blank = [&](unsigned r) {
        const Sprite* s = spd->Find(r);
        Image im;
        if (!s || !SpriteImage(romfs, "EN/MYPALACE/SOUND/MUSIC_TITLE_001.SPD", r, im))
            return true;
        uint32_t x0 = im.w, x1 = 0;
        for (uint32_t y = 0; y < im.h; ++y)
            for (uint32_t x = 0; x < im.w; ++x)
                if (im.rgba[(size_t(y) * im.w + x) * 4 + 3]) {
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x + 1);
                }
        return x1 <= x0 || x1 - x0 < 8;
    };
    struct Row {
        unsigned row;
        std::string key, text;
        bool art;
    };
    std::map<uint32_t, std::vector<Row>> titled;
    for (unsigned r = 0; r < rows.size() && r < 256; ++r) {
        if (blank(r))
            continue;
        std::string text = rows[r].name;
        while (!text.empty() && text.back() == ' ')
            text.pop_back();
        const bool art = r >= PlaceholderRows;
        titled[rows[r].cue].push_back(
            {r, art ? "row:" + std::to_string(r) : text, art ? std::string{} : text, art});
    }
    std::map<uint32_t, std::vector<uint32_t>> by_wave;
    for (const auto& [cue, w] : waves)
        if (cue < MaxBgmCue)
            by_wave[w].push_back(cue);
    for (const auto& [cue, w] : waves) {
        if (cue >= MaxBgmCue)
            continue;
        if (auto it = titled.find(cue); it != titled.end()) {
            if (it->second.size() != 1)
                return false; // the data no longer matches the one-row-per-cue rule
            const Row& t = it->second[0];
            out.push_back({uint16_t(cue), uint8_t(t.row), false, t.art, t.text});
            continue;
        }
        if (w == SilentWaveform)
            continue;
        std::set<std::string> keys;
        const Row* best = nullptr;
        for (uint32_t peer : by_wave[w]) {
            const auto p = titled.find(peer);
            if (p == titled.end())
                continue;
            for (const Row& t : p->second) {
                keys.insert(t.key);
                if (!best || t.row < best->row)
                    best = &t;
            }
        }
        if (keys.size() == 1 && best)
            out.push_back({uint16_t(cue), uint8_t(best->row), true, best->art, best->text});
    }
    return true;
}

// ================================================================================ debug
bool IsDecoderKey(std::string_view key) {
    return key.starts_with("module:p5r_dec:");
}

namespace {
void Log(const EdenDsmodHostApi& host, const std::string& s) {
    if (host.log)
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, s.c_str());
}
bool SplitLast(std::string_view s, std::string_view& path, uint32_t& n) {
    const size_t c = s.rfind(':');
    if (c == std::string_view::npos)
        return false;
    path = s.substr(0, c);
    const auto num = s.substr(c + 1);
    if (num.empty() || num.size() > 9)
        return false;
    n = 0;
    for (char ch : num) {
        if (ch < '0' || ch > '9')
            return false;
        n = n * 10 + uint32_t(ch - '0');
    }
    return !path.empty();
}

void Probe(Romfs& romfs, const EdenDsmodHostApi& host) {
    char buf[512];
    const ReadFn read = HostReader(host);
    for (const char* p :
         {"CPK/ALL_USEU.CPK", "CPK/PATCH1.CPK", "CPK/PATCH2.CPK", "CPK/PATCH.CPK", "CPK/EN.CPK",
          "CPK/BASE.CPK", "app_config/fiber_version.txt", "app_config/region_useu.txt"}) {
        std::snprintf(buf, sizeof(buf), "P5R af_dec probe: romfs '%s' size %zu", p,
                      read ? read(p, 0, nullptr, 0) : size_t(0));
        Log(host, buf);
    }
    Log(host, "P5R af_dec probe: " + romfs.Describe());
    for (size_t i = 0; i < romfs.Patch().Count(); ++i) {
        std::string path;
        Cpk::Entry e;
        romfs.Patch().At(i, path, e);
        std::snprintf(buf, sizeof(buf), "P5R af_dec probe: PATCH1 entry '%s' %llu/%llu",
                      path.c_str(), (unsigned long long)e.size, (unsigned long long)e.extract);
        Log(host, buf);
    }
    // Checksums of the files the package needs (compare with the local unpack offline).
    for (const char* p :
         {"EN/INIT/P5CAMP_00SPD.SPD", "EN/FONT/ICON.DDS", "EN/FONT/FONT0.FNT",
          "EN/CAMP/CHARATEX/C_CHARA_02.DDS", "EN/CAMP/CARDTEX/C_CARD00.DDS",
          "BASE/FIELD/PANEL/ROADMAP/RMAP_001_1_0.DDS", "EN/FIELD/PANEL/ROADMAP/ROADMAP.TBL",
          "EN/BATTLE/TABLE/NAME.TBL", "EN/INIT/MYPTABLE.BIN", "EN/INIT/CMM.BIN",
          "BASE/SOUND/BGM.ACB", "EN/MYPALACE/SOUND/MUSIC_TITLE_001.SPD",
          "EN/BATTLE/GUI/P5_BATTLE_PARTYPANEL.SPD", "MOVIE/MOV000.USM"}) {
        std::vector<uint8_t> bytes;
        const auto t0 = Clock::now();
        const bool ok = romfs.Read(p, bytes);
        std::snprintf(buf, sizeof(buf),
                      "P5R af_dec probe: read '%s' %s %zu bytes fnv %016llx in %.1f ms", p,
                      ok ? "ok" : "MISSING", bytes.size(),
                      (unsigned long long)(ok ? dsmod_sdk::Fnv1a64(bytes) : 0), Ms(t0));
        Log(host, buf);
    }
}
} // namespace

bool LoadDecoderImage(Romfs& romfs, const EdenDsmodHostApi& host, std::string_view key,
                      Image& out) {
    if (!IsDecoderKey(key))
        return false;
    std::string_view rest = key.substr(std::string_view("module:p5r_dec:").size());
    if (!romfs.IsOpen() && !romfs.Open(host)) {
        Log(host, "P5R af_dec: romfs open failed: " + romfs.Error());
        return false;
    }
    const auto t0 = Clock::now();
    bool ok = false;
    std::string_view path;
    uint32_t n = 0;
    if (rest == "probe") {
        Probe(romfs, host);
        out.w = out.h = 1;
        out.rgba.assign(4, 0);
        ok = true;
    } else if (rest.starts_with("sprite:")) {
        ok = SplitLast(rest.substr(7), path, n) && SpriteImage(romfs, path, n, out);
    } else if (rest.starts_with("tex:")) {
        ok = SplitLast(rest.substr(4), path, n) && SpdTextureImage(romfs, path, n, out);
    } else if (rest.starts_with("dds:")) {
        ok = DdsImage(romfs, rest.substr(4), out);
    } else if (rest.starts_with("icon:")) {
        ok = SplitLast("x:" + std::string(rest.substr(5)), path, n) && IconCell(romfs, int(n), out);
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf), "P5R af_dec: %s '%.*s' %ux%u in %.1f ms (cache %.1f MiB)",
                  ok ? "decoded" : "FAILED", int(key.size()), key.data(), out.w, out.h, Ms(t0),
                  romfs.CacheBytes() / 1048576.0);
    Log(host, buf);
    return ok;
}

} // namespace p5r_assets
