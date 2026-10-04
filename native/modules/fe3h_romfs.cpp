// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe3h_romfs.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "dread_mapgen_util.h"

namespace fe3h_romfs {
namespace {

uint32_t U32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint64_t U64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
constexpr uint64_t Align80(uint64_t v) {
    return (v + 0x7F) & ~uint64_t{0x7F};
}
bool InRange(uint64_t off, uint64_t size, uint64_t total) {
    return off <= total && size <= total - off;
}

// A romfs path from a patch table: "rom:/patch4/x/y.bin" -> "patch4/x/y.bin". Rejects anything
// that could leave the romfs (.., backslashes, empty components).
bool RomPath(const char* raw, size_t cap, std::string& out) {
    const size_t len = strnlen(raw, cap);
    if (len == cap)
        return false;
    std::string_view s{raw, len};
    if (!s.starts_with("rom:/"))
        return false;
    s.remove_prefix(5);
    if (s.empty() || s.find("..") != std::string_view::npos || s.find('\\') != std::string_view::npos ||
        s.find("//") != std::string_view::npos || s.front() == '/')
        return false;
    for (const char c : s)
        if (static_cast<unsigned char>(c) < 0x20)
            return false;
    out.assign(s);
    return true;
}

class FileSource final : public Source {
public:
    FileSource(ReadFn read, std::string path, uint64_t offset, uint64_t size)
        : read_(std::move(read)), path_(std::move(path)), offset_(offset), size_(size) {}
    uint64_t Size() const override {
        return size_;
    }
    bool Read(uint64_t offset, size_t size, uint8_t* out) override {
        if (!InRange(offset, size, size_))
            return false;
        return size == 0 || read_(path_.c_str(), offset_ + offset, out, size) == size;
    }

private:
    ReadFn read_;
    std::string path_;
    uint64_t offset_, size_;
};

class SubSource final : public Source {
public:
    SubSource(SourcePtr inner, uint64_t offset, uint64_t size)
        : inner_(std::move(inner)), offset_(offset), size_(size) {}
    uint64_t Size() const override {
        return size_;
    }
    bool Read(uint64_t offset, size_t size, uint8_t* out) override {
        return InRange(offset, size, size_) && inner_->Read(offset_ + offset, size, out);
    }

private:
    SourcePtr inner_;
    uint64_t offset_, size_;
};

class MemorySource final : public Source {
public:
    explicit MemorySource(std::vector<uint8_t> b) : b_(std::move(b)) {}
    uint64_t Size() const override {
        return b_.size();
    }
    bool Read(uint64_t offset, size_t size, uint8_t* out) override {
        if (!InRange(offset, size, b_.size()))
            return false;
        if (size)
            std::memcpy(out, b_.data() + offset, size);
        return true;
    }

private:
    std::vector<uint8_t> b_;
};

struct KtHeader {
    uint32_t chunk{}, count{}, total{};
    std::vector<uint32_t> stored;
    std::vector<uint64_t> start; // chunk positions in the container
};

// Validates a KT-gz header against the container size.
bool ParseKtHeader(const uint8_t* head, size_t head_len, uint64_t container, KtHeader& h,
                   const std::function<bool(uint64_t, size_t, uint8_t*)>& more) {
    if (head_len < 12)
        return false;
    h.chunk = U32(head);
    h.count = U32(head + 4);
    h.total = U32(head + 8);
    const uint32_t cs = h.chunk, n = h.count;
    if (cs < 0x1000 || cs > 0x1000000 || (cs & (cs - 1)) || n == 0 || n > 0x10000 ||
        h.total == 0 || h.total > MaxBlob)
        return false;
    if (uint64_t(n - 1) * cs >= h.total || uint64_t(n) * cs < h.total)
        return false;
    const uint64_t table_end = 12 + uint64_t(n) * 4;
    if (table_end > container)
        return false;
    h.stored.resize(n);
    if (table_end <= head_len) {
        std::memcpy(h.stored.data(), head + 12, size_t(n) * 4);
    } else if (!more(12, size_t(n) * 4, reinterpret_cast<uint8_t*>(h.stored.data()))) {
        return false;
    }
    h.start.resize(n);
    uint64_t p = Align80(table_end);
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t plain = std::min<uint64_t>(cs, h.total - uint64_t(i) * cs);
        const uint32_t s = h.stored[i];
        if (s == 0 || !InRange(p, s, container))
            return false;
        if (s != plain && s < 4 + 6) // zlib length + a minimal zlib stream
            return false;
        h.start[i] = p;
        p = Align80(p + s);
    }
    return true;
}

// Inflates one chunk (raw or {u32 zlib length, zlib}) into out (exactly `plain` bytes).
bool DecodeChunk(std::span<const uint8_t> c, size_t plain, std::vector<uint8_t>& out) {
    out.clear();
    if (c.size() == plain) {
        out.assign(c.begin(), c.end());
        return true;
    }
    if (c.size() < 4)
        return false;
    const uint32_t zlen = U32(c.data());
    if (zlen > c.size() - 4)
        return false;
    return ZlibInflate(c.subspan(4, zlen), out, plain, plain);
}

class KtGzSource final : public Source {
public:
    KtGzSource(SourcePtr inner, KtHeader h) : inner_(std::move(inner)), h_(std::move(h)) {}
    uint64_t Size() const override {
        return h_.total;
    }
    bool Read(uint64_t offset, size_t size, uint8_t* out) override {
        if (!InRange(offset, size, h_.total))
            return false;
        while (size) {
            const uint32_t k = static_cast<uint32_t>(offset / h_.chunk);
            if (!Load(k))
                return false;
            const uint64_t in_chunk = offset - uint64_t(k) * h_.chunk;
            const size_t n = static_cast<size_t>(std::min<uint64_t>(size, cached_.size() - in_chunk));
            std::memcpy(out, cached_.data() + in_chunk, n);
            out += n;
            offset += n;
            size -= n;
        }
        return true;
    }

private:
    bool Load(uint32_t k) {
        if (k >= h_.count)
            return false;
        if (cached_index_ == static_cast<int64_t>(k))
            return true;
        cached_index_ = -1;
        std::vector<uint8_t> raw;
        if (!inner_->ReadVec(h_.start[k], h_.stored[k], raw))
            return false;
        const size_t plain = static_cast<size_t>(
            std::min<uint64_t>(h_.chunk, h_.total - uint64_t(k) * h_.chunk));
        if (!DecodeChunk(raw, plain, cached_))
            return false;
        cached_index_ = k;
        return true;
    }

    SourcePtr inner_;
    KtHeader h_;
    std::vector<uint8_t> cached_;
    int64_t cached_index_ = -1;
};

uint32_t Adler32(std::span<const uint8_t> d) {
    uint32_t a = 1, b = 0;
    size_t i = 0;
    while (i < d.size()) {
        const size_t n = std::min<size_t>(d.size() - i, 5552);
        for (size_t k = 0; k < n; ++k) {
            a += d[i + k];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        i += n;
    }
    return (b << 16) | a;
}

} // namespace

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
            const size_t got = fn(user, path, offset + done, static_cast<uint8_t*>(out) + done, want);
            done += got;
            if (got != want)
                break;
        }
        return done;
    };
}

bool Source::ReadVec(uint64_t offset, size_t size, std::vector<uint8_t>& out) {
    if (size > MaxBlob || !InRange(offset, size, Size()))
        return false;
    out.resize(size);
    return Read(offset, size, out.data());
}

bool Source::ReadAll(std::vector<uint8_t>& out, uint64_t max) {
    const uint64_t n = Size();
    if (n > max)
        return false;
    return ReadVec(0, static_cast<size_t>(n), out);
}

SourcePtr FileRange(ReadFn read, std::string path, uint64_t offset, uint64_t size) {
    if (!read || size > std::numeric_limits<uint64_t>::max() - offset)
        return nullptr;
    return std::make_shared<FileSource>(std::move(read), std::move(path), offset, size);
}
SourcePtr SubRange(SourcePtr inner, uint64_t offset, uint64_t size) {
    if (!inner || !InRange(offset, size, inner->Size()))
        return nullptr;
    return std::make_shared<SubSource>(std::move(inner), offset, size);
}
SourcePtr Memory(std::vector<uint8_t> bytes) {
    return std::make_shared<MemorySource>(std::move(bytes));
}

SourcePtr KtGz(SourcePtr inner) {
    if (!inner)
        return nullptr;
    const uint64_t size = inner->Size();
    uint8_t head[0x200];
    const size_t n = static_cast<size_t>(std::min<uint64_t>(sizeof(head), size));
    if (n < 16 || !inner->Read(0, n, head))
        return nullptr;
    KtHeader h;
    auto more = [&inner](uint64_t off, size_t len, uint8_t* out) {
        return inner->Read(off, len, out);
    };
    if (!ParseKtHeader(head, n, size, h, more))
        return nullptr;
    return std::make_shared<KtGzSource>(std::move(inner), std::move(h));
}

bool LooksKtGz(std::span<const uint8_t> d) {
    KtHeader h;
    auto more = [&d](uint64_t off, size_t len, uint8_t* out) {
        if (!InRange(off, len, d.size()))
            return false;
        std::memcpy(out, d.data() + off, len);
        return true;
    };
    return ParseKtHeader(d.data(), d.size(), d.size(), h, more);
}

bool KtGzDecompress(std::span<const uint8_t> in, std::vector<uint8_t>& out, uint64_t max) {
    KtHeader h;
    auto more = [&in](uint64_t off, size_t len, uint8_t* o) {
        if (!InRange(off, len, in.size()))
            return false;
        std::memcpy(o, in.data() + off, len);
        return true;
    };
    if (!ParseKtHeader(in.data(), in.size(), in.size(), h, more) || h.total > max)
        return false;
    out.clear();
    out.reserve(h.total);
    std::vector<uint8_t> chunk;
    for (uint32_t k = 0; k < h.count; ++k) {
        const size_t plain =
            static_cast<size_t>(std::min<uint64_t>(h.chunk, h.total - uint64_t(k) * h.chunk));
        if (!DecodeChunk(in.subspan(h.start[k], h.stored[k]), plain, chunk))
            return false;
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    return out.size() == h.total;
}

bool ZlibInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t expect,
                 size_t max) {
    out.clear();
    if (in.size() < 2 + 4 || expect > max)
        return false;
    const uint8_t cmf = in[0], flg = in[1];
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20))
        return false;
    if (expect)
        out.reserve(expect);
    // bounded while inflating: a corrupt chunk fails at the cap instead of allocating its whole
    // expansion (an Android OOM kill is not a catchable bad_alloc)
    if (!dread_mapgen::Inflate(in.subspan(2, in.size() - 2 - 4), out,
                               expect ? std::min(expect, max) : max))
        return false;
    if (out.size() > max || (expect && out.size() != expect))
        return false;
    const uint8_t* t = in.data() + in.size() - 4;
    const uint32_t adler = (uint32_t(t[0]) << 24) | (uint32_t(t[1]) << 16) | (uint32_t(t[2]) << 8) | t[3];
    return Adler32(out) == adler;
}

bool PtblCount(const SourcePtr& container, uint32_t& count) {
    uint8_t b[4];
    if (!container || container->Size() < 4 || !container->Read(0, 4, b))
        return false;
    count = U32(b);
    return uint64_t(count) * 8 + 4 <= container->Size();
}

SourcePtr PtblChild(const SourcePtr& container, uint32_t index, uint32_t* count) {
    uint32_t n{};
    if (!PtblCount(container, n))
        return nullptr;
    if (count)
        *count = n;
    if (index >= n)
        return nullptr;
    uint8_t e[8];
    if (!container->Read(4 + uint64_t(index) * 8, 8, e))
        return nullptr;
    return SubRange(container, U32(e), U32(e + 4));
}

bool LinkData::Open() {
    if (tried_)
        return open_;
    tried_ = true;
    if (!read_)
        return false;
    const size_t d0 = read_("DATA0.bin", 0, nullptr, 0);
    data1_size_ = read_("DATA1.bin", 0, nullptr, 0);
    if (d0 == 0 || d0 % 0x20 || d0 > (size_t(1) << 24) || data1_size_ == 0)
        return false;
    std::vector<uint8_t> t(d0);
    if (read_("DATA0.bin", 0, t.data(), d0) != d0)
        return false;
    entries_.resize(d0 / 0x20);
    for (size_t i = 0; i < entries_.size(); ++i) {
        const uint8_t* r = t.data() + i * 0x20;
        EntryInfo& e = entries_[i];
        e.offset = U64(r);
        e.size = U64(r + 8);
        e.stored = U64(r + 16);
        e.compressed = U64(r + 24) != 0;
        if (e.size && !InRange(e.offset, e.stored, data1_size_))
            e = EntryInfo{}; // fail closed for this id only
    }
    // The newest patch directory that carries INFO2 holds the cumulative override table.
    for (int n = 9; n >= 1; --n) {
        const std::string dir = "patch" + std::to_string(n);
        uint8_t i2[16];
        if (read_((dir + "/INFO2.bin").c_str(), 0, nullptr, 0) < 16 ||
            read_((dir + "/INFO2.bin").c_str(), 0, i2, 16) != 16)
            continue;
        const uint64_t n0 = U64(i2);
        const std::string info0 = dir + "/INFO0.bin";
        const size_t size0 = read_(info0.c_str(), 0, nullptr, 0);
        if (n0 > 0x100000 || n0 * 0x120 > size0)
            break; // present but inconsistent: use the base archive only
        std::vector<uint8_t> i0(static_cast<size_t>(n0 * 0x120));
        if (!i0.empty() && read_(info0.c_str(), 0, i0.data(), i0.size()) != i0.size())
            break;
        patch_dir_ = dir;
        for (uint64_t k = 0; k < n0; ++k) {
            const uint8_t* r = i0.data() + k * 0x120;
            const uint64_t id = U64(r);
            EntryInfo e;
            e.size = U64(r + 8);
            e.stored = U64(r + 16);
            e.compressed = U64(r + 24) != 0;
            if (id >= entries_.size() ||
                !RomPath(reinterpret_cast<const char*>(r + 0x20), 0x100, e.path))
                continue;
            entries_[id] = std::move(e);
            ++patched_;
        }
        // INFO1: files with no DATA0 slot; the game addresses record k as id DATA0 count + k
        // (e.g. 0x7B23 = 31160 + 363 in the still loader 0x69F240 = still/face_btl_l_cmn_DLC.bin).
        const uint64_t n1 = U64(i2 + 8);
        const std::string info1 = dir + "/INFO1.bin";
        const size_t size1 = read_(info1.c_str(), 0, nullptr, 0);
        if (n1 > 0 && n1 <= 0x100000 && n1 * 0x118 <= size1) {
            std::vector<uint8_t> i1(static_cast<size_t>(n1 * 0x118));
            if (read_(info1.c_str(), 0, i1.data(), i1.size()) == i1.size()) {
                extra_.resize(static_cast<size_t>(n1));
                for (uint64_t k = 0; k < n1; ++k) {
                    const uint8_t* r = i1.data() + k * 0x118;
                    EntryInfo e;
                    e.size = U64(r);
                    e.stored = U64(r + 8);
                    e.compressed = U64(r + 16) != 0;
                    if (RomPath(reinterpret_cast<const char*>(r + 0x18), 0x100, e.path))
                        extra_[k] = std::move(e);
                }
            }
        }
        break;
    }
    open_ = true;
    return true;
}

const EntryInfo* LinkData::Info(uint32_t id) const {
    if (!open_)
        return nullptr;
    if (id >= entries_.size()) {
        const size_t k = id - entries_.size();
        return k < extra_.size() && extra_[k].size != 0 && !extra_[k].path.empty() ? &extra_[k]
                                                                                   : nullptr;
    }
    if (entries_[id].size == 0)
        return nullptr;
    return &entries_[id];
}

SourcePtr LinkData::Entry(uint32_t id) {
    if (!Open())
        return nullptr;
    const EntryInfo* e = Info(id);
    if (!e || e->stored == 0)
        return nullptr;
    SourcePtr raw;
    if (e->path.empty()) {
        raw = FileRange(read_, "DATA1.bin", e->offset, e->stored);
    } else {
        const size_t file = read_(e->path.c_str(), 0, nullptr, 0);
        if (file < e->stored)
            return nullptr;
        raw = FileRange(read_, e->path, 0, e->stored);
    }
    if (!e->compressed)
        return e->stored == e->size ? raw : nullptr;
    SourcePtr plain = KtGz(std::move(raw));
    if (!plain || plain->Size() != e->size)
        return nullptr;
    return plain;
}

} // namespace fe3h_romfs
