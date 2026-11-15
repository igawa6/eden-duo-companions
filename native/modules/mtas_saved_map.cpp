// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mtas_saved_map.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 1024
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

namespace mtas {
namespace {
// PC saves carry a 1024 px mask; Switch V1.4.2 saves carry the 256 px render texture itself.
bool SupportedSide(int side) { return side == 256 || side == 512 || side == 1024; }
std::uint32_t Crc32(const std::uint8_t* p, std::size_t n) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xffffffffu;
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}
struct Cursor {
    std::span<const std::uint8_t> data;
    std::span<const std::uint8_t> Take(std::size_t n) {
        if (n > data.size()) throw std::runtime_error("truncated save");
        auto out = data.first(n);
        data = data.subspan(n);
        return out;
    }
    std::uint32_t U32() {
        const auto b = Take(4);
        return b[0] | std::uint32_t{b[1]} << 8 | std::uint32_t{b[2]} << 16 |
               std::uint32_t{b[3]} << 24;
    }
    void Bool() { if (Take(1)[0] > 1) throw std::runtime_error("save boolean"); }
    std::string Name() {
        std::uint32_t n{};
        for (unsigned shift = 0; shift < 35; shift += 7) {
            const auto b = Take(1)[0];
            if (shift == 28 && (b & 0xf0)) throw std::runtime_error("save name length");
            n |= std::uint32_t{b & 127u} << shift;
            if (!(b & 128)) {
                if (n == 0 || n > 256) throw std::runtime_error("save name size");
                auto s = Take(n);
                if (std::find(s.begin(), s.end(), 0) != s.end())
                    throw std::runtime_error("save name nul");
                return {s.begin(), s.end()};
            }
        }
        throw std::runtime_error("save name continuation");
    }
    void End() { if (!data.empty()) throw std::runtime_error("save trailing bytes"); }
};
}

std::optional<ce_unity::Image> DecodeSavedFog(std::span<const std::uint8_t> save) {
    try {
        if (save.size() < 24 || save.size() > SavedMapMaxSave) return std::nullopt;
        Cursor header{save};
        const auto declared = header.U32(), schema = header.U32();
        header.Take(8); // native timestamp, not an exploration mask
        const auto compressed = header.U32();
        if ((schema != 9 && schema != 10) || compressed > 1) return std::nullopt;
        std::vector<std::uint8_t> inflated;
        auto raw = header.data;
        if (compressed) {
            // Single gzip member: header, raw deflate, CRC32 + ISIZE trailer. stb_image's own
            // inflate into a fixed buffer of the declared size (no growth, no zlib dependency).
            Cursor gz{raw};
            const auto id = gz.Take(4);
            if (id[0] != 0x1f || id[1] != 0x8b || id[2] != 8 || (id[3] & 0xe0)) return std::nullopt;
            const auto flags = id[3];
            gz.Take(6);
            if (flags & 4) { const auto x = gz.Take(2); gz.Take(x[0] | std::size_t{x[1]} << 8); }
            for (const std::uint8_t text : {std::uint8_t{8}, std::uint8_t{16}})
                if (flags & text) while (gz.Take(1)[0] != 0) {}
            if (flags & 2) gz.Take(2);
            if (gz.data.size() < 8) return std::nullopt;
            // The 8 trailer bytes stay in the inflate input: stb's bit reader prefetches past
            // the final block and refuses a stream that ends exactly at the last deflate byte.
            const auto body = gz.data;
            Cursor trailer{gz.data.last(8)};
            const auto crc = trailer.U32(), length = trailer.U32();
            if (length == 0 || length > SavedMapMaxSave) return std::nullopt;
            inflated.resize(length);
            const int got = stbi_zlib_decode_noheader_buffer(
                reinterpret_cast<char*>(inflated.data()), static_cast<int>(length),
                reinterpret_cast<const char*>(body.data()), static_cast<int>(body.size()));
            if (got < 0 || static_cast<std::uint32_t>(got) != length ||
                Crc32(inflated.data(), inflated.size()) != crc) return std::nullopt;
            raw = inflated;
        }
        if (compressed ? raw.size() < 4 || declared != raw.size() - 4
                       : declared != raw.size() + 16) return std::nullopt;
        Cursor modules{raw};
        const auto count = modules.U32();
        if (count == 0 || count > 512) return std::nullopt;
        std::set<std::string> names;
        std::span<const std::uint8_t> encoded;
        for (unsigned i = 0; i < count; ++i) {
            const auto name = modules.Name();
            if (!names.insert(name).second) return std::nullopt;
            Cursor body{modules.Take(modules.U32())};
            if (name != "Pathea.MapNs.MapModule") continue;
            const auto version = body.U32(), length = body.U32();
            if (version != 1 || length != body.data.size()) return std::nullopt;
            body.Bool();
            if (body.U32() != 9) return std::nullopt;
            for (int j = 0; j < 9; ++j) body.Bool();
            const auto size = body.U32();
            if (size == 0 || size > 4u * 1024u * 1024u) return std::nullopt;
            encoded = body.Take(size);
            const auto enables = body.U32();
            if (enables > 512) return std::nullopt;
            for (unsigned j = 0; j < enables; ++j) { body.U32(); body.Bool(); }
            body.End();
        }
        modules.End();
        if (encoded.size() < 8) return std::nullopt;
        const bool jpeg = encoded[0] == 0xff && encoded[1] == 0xd8;
        constexpr std::uint8_t png[]{137,80,78,71,13,10,26,10};
        if (!jpeg && std::memcmp(encoded.data(), png, 8) != 0) return std::nullopt;
        if (jpeg && (encoded[encoded.size() - 2] != 0xff || encoded.back() != 0xd9))
            return std::nullopt;
        if (!jpeg) {
            // The image decoder does not validate PNG CRCs. Validate every chunk
            // and require an exact IEND, so corruption cannot become exploration.
            Cursor chunks{encoded.subspan(8)};
            const auto big = [](std::span<const std::uint8_t> b) {
                return std::uint32_t{b[0]} << 24 | std::uint32_t{b[1]} << 16 |
                       std::uint32_t{b[2]} << 8 | b[3];
            };
            bool ended = false;
            while (!chunks.data.empty()) {
                const auto n = big(chunks.Take(4));
                if (chunks.data.size() < 8 || n > chunks.data.size() - 8) return std::nullopt;
                const auto payload = chunks.Take(std::size_t{n} + 4);
                const auto checksum = big(chunks.Take(4));
                if (Crc32(payload.data(), payload.size()) != checksum) return std::nullopt;
                if (std::memcmp(payload.data(), "IEND", 4) == 0) {
                    if (n != 0) return std::nullopt;
                    chunks.End(); ended = true; break;
                }
            }
            if (!ended) return std::nullopt;
        }
        int w{}, h{}, components{};
        if (!stbi_info_from_memory(encoded.data(), static_cast<int>(encoded.size()), &w, &h,
                                   &components) || w != h || !SupportedSide(w)) return std::nullopt;
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{
            stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                                  &w, &h, &components, 4), &stbi_image_free};
        if (!pixels || w != h || !SupportedSide(w)) return std::nullopt;
        ce_unity::Image image;
        image.width = w; image.height = h;
        image.rgba.assign(pixels.get(), pixels.get() + std::size_t(w) * h * 4);
        return image;
    } catch (...) { return std::nullopt; }
}

void SavedMapCache::Clear() {
    if (fog_) ++revision_;
    fog_.reset(); bytes_.clear(); path_.clear();
}

bool SavedMapCache::Poll(const std::string& path, bool reset, const SaveMapRead& read) {
    const auto before = revision_;
    try {
        if (reset || path != path_) Clear();
        path_ = path;
        const auto fail = [&] { Clear(); return revision_ != before; };
        if (!read || path.empty() || path.size() > 256 || path.front() == '/' ||
            path.find("..") != std::string::npos || path.find('\\') != std::string::npos ||
            path.find('\0') != std::string::npos)
            return fail();
        const auto size = read(path.c_str(), 0, nullptr, 0);
        if (size < 24 || size > SavedMapMaxSave) return fail();
        std::vector<std::uint8_t> bytes(size);
        if (read(path.c_str(), 0, bytes.data(), size) != size ||
            read(path.c_str(), 0, nullptr, 0) != size) return fail();
        // Exact bytes rather than a collision-prone fingerprint. Unchanged saves are
        // not decoded or republished. Two reads reject same-size replacement mid-read.
        std::vector<std::uint8_t> confirmation(size);
        if (read(path.c_str(), 0, confirmation.data(), size) != size || confirmation != bytes)
            return fail();
        if (bytes == bytes_ && fog_) return revision_ != before;
        auto fog = DecodeSavedFog(bytes);
        if (!fog) return fail();
        bytes_ = std::move(bytes);
        if (!fog_ || fog_->rgba != fog->rgba) { fog_ = std::move(fog); ++revision_; }
        return revision_ != before;
    } catch (...) {
        Clear();
        return revision_ != before;
    }
}
} // namespace mtas
