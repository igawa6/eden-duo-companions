// SPDX-License-Identifier: GPL-3.0-or-later
#include "smo_map.h"
#include "smo_save.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>
#include <utility>

namespace SmoMap {
FooterLayout LayoutForMoonSlots(int slots) {
    FooterLayout out;
    out.large = slots <= 20;
    const int count = std::clamp(slots, 0, 40);
    const int columns = out.large ? 10 : 15;
    out.rows = (count + columns - 1) / columns;
    out.variant = out.large ? out.rows : out.rows + 1;
    out.footer_y = 968 - (out.large ? 88 : 56) * out.rows;
    out.map_height = out.footer_y - 112;
    return out;
}
namespace {
using Span = std::span<const uint8_t>;
bool Fits(Span b, uint64_t p, uint64_t n) { return p <= b.size() && n <= b.size() - p; }
uint16_t U16(Span b, size_t p) { return uint16_t(b[p]) | uint16_t(b[p + 1]) << 8; }
uint32_t U32(Span b, size_t p) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= uint32_t(b[p + i]) << (8 * i);
    return v;
}
uint64_t U64(Span b, size_t p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= uint64_t(b[p + i]) << (8 * i);
    return v;
}
// Bounded port of mk8d_assets.cpp's Yaz0 recipe; overlapping backreferences are valid.
constexpr size_t MaxArchive = 64 * 1024 * 1024;
std::optional<std::vector<uint8_t>> ExpandYaz0(Span in) {
    if (in.size() < 16) return std::nullopt;
    const uint32_t size = uint32_t(in[4]) << 24 | uint32_t(in[5]) << 16 |
                          uint32_t(in[6]) << 8 | uint32_t(in[7]);
    if (!size || size > MaxArchive) return std::nullopt;
    std::vector<uint8_t> out(size);
    size_t si = 16, di = 0;
    uint8_t code = 0, mask = 0;
    while (di < out.size()) {
        if (!mask) {
            if (si >= in.size()) return std::nullopt;
            code = in[si++]; mask = 0x80;
        }
        if (code & mask) {
            if (si >= in.size()) return std::nullopt;
            out[di++] = in[si++];
        } else {
            if (!Fits(in, si, 2)) return std::nullopt;
            const uint8_t a = in[si++], b = in[si++];
            const size_t distance = ((a & 15) << 8) | b;
            if (distance + 1 > di) return std::nullopt;
            size_t count = a >> 4;
            if (!count) {
                if (si >= in.size()) return std::nullopt;
                count = size_t(in[si++]) + 0x12;
            } else {
                count += 2;
            }
            if (count > out.size() - di) return std::nullopt;
            size_t source = di - distance - 1;
            while (count--) out[di++] = out[source++];
        }
        mask >>= 1;
    }
    return out;
}
// BFRES 8.0 external-file records, with file-relative offsets. Never search for magic.
std::optional<Span> EmbeddedBntx(Span b) {
    if (b.size() < 0xd0 || std::memcmp(b.data(), "FRES", 4) ||
        U32(b, 8) != 0x00080000 || U16(b, 0xc) != 0xfeff || U32(b, 0x1c) != b.size())
        return std::nullopt;
    const auto table = U64(b, 0x98);
    const auto count = U16(b, 0xc8);
    if (!count || count > 64 || !Fits(b, table, uint64_t(count) * 16))
        return std::nullopt;
    std::optional<Span> found;
    for (unsigned i = 0; i < count; ++i) {
        const auto p = U64(b, size_t(table) + i * 16);
        const auto n = U64(b, size_t(table) + i * 16 + 8);
        if (n < 4 || !Fits(b, p, n)) return std::nullopt;
        if (!std::memcmp(b.data() + p, "BNTX", 4)) {
            if (found) return std::nullopt;
            found = b.subspan(size_t(p), size_t(n));
        }
    }
    return found;
}
} // namespace

std::optional<Resources> DecodeProjection(Span byml, uint32_t width, uint32_t height) {
    if (!width || !height || width > 4096 || height > 4096) return std::nullopt;
    const auto root = SmoSave::ParseByml(byml);
    if (!root) return std::nullopt;
    const auto* values = root->Find("ViewProjMatrix");
    if (!values || !values->IsArray() || values->arr.size() != 16) return std::nullopt;
    Resources out;
    out.image.width = width; out.image.height = height;
    for (size_t i = 0; i < 16; ++i) {
        const auto& v = values->arr[i];
        if (v.type != SmoSave::Node::Type::Float || !std::isfinite(v.f))
            return std::nullopt;
        out.view_proj[i] = v.f;
    }
    if (!Project(out, {0, 0, 0})) return std::nullopt;
    return out;
}

std::optional<Resources> DecodeArchive(Span archive, std::string_view stage) {
    if (stage.empty() || stage.size() > 128 || stage.find_first_of("/\\#") != stage.npos)
        return std::nullopt;
    std::vector<uint8_t> expanded;
    if (archive.size() >= 4 && !std::memcmp(archive.data(), "Yaz0", 4)) {
        auto decoded = ExpandYaz0(archive);
        if (!decoded) return std::nullopt;
        expanded = std::move(*decoded);
        archive = expanded;
    }
    if (archive.size() > MaxArchive) return std::nullopt;
    Span bfres, matrix;
    for (const auto& f : fe3h_ui2d::Sarc(archive)) {
        if (!Fits(archive, f.offset, f.size)) return std::nullopt;
        if (f.name == "Texture2dMap.bfres") {
            if (!bfres.empty()) return std::nullopt;
            bfres = archive.subspan(f.offset, f.size);
        }
        if (f.name == std::string(stage) + ".byml") {
            if (!matrix.empty()) return std::nullopt;
            matrix = archive.subspan(f.offset, f.size);
        }
    }
    const auto bntx = EmbeddedBntx(bfres);
    if (!bntx) return std::nullopt;
    auto image = fe3h_ui2d::BntxDecode(*bntx, stage);
    if (!image) return std::nullopt;
    auto out = DecodeProjection(matrix, image->width, image->height);
    if (!out) return std::nullopt;
    out->image = std::move(*image);
    return out;
}

std::optional<Point> Project(const Resources& r, const std::array<float, 3>& world) {
    if (!r.image.width || !r.image.height) return std::nullopt;
    for (auto v : world) if (!std::isfinite(v)) return std::nullopt;
    for (auto v : r.view_proj) if (!std::isfinite(v)) return std::nullopt;
    const auto& m = r.view_proj;
    const auto row = [&](size_t n) {
        return m[n] * world[0] + m[n + 1] * world[1] + m[n + 2] * world[2] + m[n + 3];
    };
    const double w = row(12);
    if (!std::isfinite(w) || std::abs(w) < 1e-12) return std::nullopt;
    Point p{(row(0) / w + 1) * r.image.width * .5,
            (row(4) / w - 1) * r.image.height * .5};
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) return std::nullopt;
    return p;
}

std::optional<double> Heading(const Resources& r, const std::array<float, 3>& world,
                              const std::array<float, 3>& front) {
    std::array<float, 3> tip;
    for (size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(front[i])) return std::nullopt;
        tip[i] = world[i] + front[i];
    }
    const auto a = Project(r, world), b = Project(r, tip);
    if (!a || !b) return std::nullopt;
    const double dx = b->x - a->x, dy = b->y - a->y;
    if (std::hypot(dx, dy) < 1e-12) return std::nullopt;
    double degrees = std::atan2(dx, dy) * 180 / std::numbers::pi;
    if (degrees < 0) degrees += 360;
    return degrees;
}

std::optional<fe3h_ui2d::Texture> DecodeFacingArc(Span bntx, unsigned degrees) {
    if (degrees >= 360) return std::nullopt;
    const auto sprite = fe3h_ui2d::BntxDecode(bntx, "NyIconView^t");
    if (!sprite || sprite->width != 64 || sprite->height != 64 ||
        sprite->rgba.size() != 64 * 64 * 4) return std::nullopt;
    fe3h_ui2d::Texture out{"NyIconView^t", 128, 128, std::vector<uint8_t>(128 * 128 * 4)};
    const double angle = degrees * std::numbers::pi / 180;
    const double c = std::cos(angle), s = std::sin(angle);
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            // The runtime's rotating live marker is above the upright player pin.
            // Keep the 56px badge clear at the cone's 160px display size.
            if ((x - 64) * (x - 64) + (y - 64) * (y - 64) < 23 * 23) continue;
            const long sx = std::lround(c * (x - 64) + s * (y - 64) + 64);
            const long sy = std::lround(-s * (x - 64) + c * (y - 64) + 64);
            if (sx < 0 || sx >= 64 || sy < 0 || sy >= 64) continue;
            std::memcpy(out.rgba.data() + (size_t(y) * 128 + x) * 4,
                        sprite->rgba.data() + (size_t(sy) * 64 + size_t(sx)) * 4, 4);
        }
    }
    return out;
}

std::optional<fe3h_ui2d::Texture> DecodePlayerPin(Span bntx) {
    auto image = fe3h_ui2d::BntxDecode(bntx, "CmIconPlayer^t");
    if (!image) return std::nullopt;
    for (size_t p = 0; p + 3 < image->rgba.size(); p += 4) {
        const bool dark = image->rgba[p] < 128;
        image->rgba[p] = dark ? 255 : 0xe6;
        image->rgba[p + 1] = dark ? 255 : 0x10;
        image->rgba[p + 2] = dark ? 255 : 0x1e;
    }
    return image;
}

std::optional<fe3h_ui2d::Texture> DecodePlayerArrow(Span bntx) {
    auto image = fe3h_ui2d::BntxDecode(bntx, "MyIconPlayerArrow^s");
    if (!image || image->width != 32 || image->height != 52) return std::nullopt;
    // Native BC4 is a coverage mask. Leave the upright 56px Mario badge clear
    // when the arrow is drawn at 2x scale around its native centre (16,26).
    for (int y=0;y<52;++y) for (int x=0;x<32;++x) {
        const size_t p=(size_t(y)*32+x)*4;
        const auto coverage=image->rgba[p];
        image->rgba[p]=image->rgba[p+1]=image->rgba[p+2]=255;
        image->rgba[p+3]=(x-16)*(x-16)+(y-26)*(y-26)<14*14?0:coverage;
    }
    return image;
}

std::optional<fe3h_ui2d::Texture> DecodeCameraConeAtlas(Span bntx) {
    // A single resident atlas avoids an asynchronous image request every time
    // the camera turns. Each cell uses the existing native cone artwork.
    fe3h_ui2d::Texture atlas{"camera_cones",1024,1152,std::vector<uint8_t>(1024*1152*4)};
    for (unsigned i=0;i<72;++i) {
        const auto arc=DecodeFacingArc(bntx,(i*5+45)%360);
        if (!arc) return std::nullopt;
        for (unsigned y=0;y<128;++y)
            std::memcpy(atlas.rgba.data()+((size_t(i/8)*128+y)*1024+(i%8)*128)*4,
                        arc->rgba.data()+size_t(y)*128*4,128*4);
    }
    return atlas;
}

Point NudgeAwayFromConflicts(const Point& candidate, const std::vector<Point>& others,
                             double min_separation, double offset) {
    Point result = candidate;
    for (const auto& other : others) {
        double dx = result.x - other.x, dy = result.y - other.y;
        const double distance = std::hypot(dx, dy);
        if (distance >= min_separation) continue;
        if (distance < 1e-6) {
            dx = -1; dy = 0; // coincident: a fixed, deterministic unit direction (up-left axis)
        } else {
            dx /= distance; dy /= distance;
        }
        result.x += dx * offset;
        result.y += dy * offset;
    }
    return result;
}

} // namespace SmoMap
