// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_spine_atlas.h"
#include "cotl_test_util.h"

namespace {

constexpr const char* kAtlas = R"(
heart.png
size: 1024,256
format: RGBA8888
filter: Linear,Linear
repeat: none
images/HeartBack
  rotate: true
  xy: 457, 60
  size: 94, 90
  orig: 115, 115
  offset: 11, 13
  index: -1
images/HeartFull_Fire
  rotate: false
  xy: 2, 24
  size: 100, 114
  orig: 100, 114
  offset: 0, 0
  index: -1
)";

// 4x3 image, pixel (x, y) has red = x + 10 * y
ce_unity::Image Grid(uint32_t w, uint32_t h) {
    ce_unity::Image img;
    img.width = w;
    img.height = h;
    img.rgba.resize(w * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            img.rgba[(y * w + x) * 4] = static_cast<uint8_t>(x + 10 * y);
            img.rgba[(y * w + x) * 4 + 3] = 255;
        }
    }
    return img;
}

uint8_t Red(const ce_unity::Image& img, uint32_t x, uint32_t y) {
    return img.rgba[(y * img.width + x) * 4];
}

} // namespace

TEST(spine_atlas_parses_regions) {
    const auto a = cotl::ParseSpineAtlas(kAtlas);
    CHECK_EQ(a.page, std::string("heart.png"));
    REQUIRE(a.regions.count("images/HeartBack") == 1);
    const auto& r = a.regions.at("images/HeartBack");
    CHECK(r.rotate);
    CHECK_EQ(r.x, 457);
    CHECK_EQ(r.y, 60);
    CHECK_EQ(r.w, 94);
    CHECK_EQ(r.h, 90);
    CHECK_EQ(r.orig_w, 115);
    CHECK_EQ(r.off_x, 11);
    CHECK_EQ(r.off_y, 13);
    CHECK(!a.regions.at("images/HeartFull_Fire").rotate);
}

TEST(spine_crop_unrotated_region) {
    cotl::SpineRegion r;
    r.x = 1;
    r.y = 1;
    r.w = 2;
    r.h = 2;
    const auto c = cotl::CropRegion(Grid(4, 3), r);
    REQUIRE(c.has_value());
    CHECK_EQ(c->width, 2u);
    CHECK_EQ(Red(*c, 0, 0), uint8_t{11});
    CHECK_EQ(Red(*c, 1, 1), uint8_t{22});
}

// Rotated region (libGDX/Spine "rotate: true"): occupies h x w in the atlas, stored rotated 90 degrees
// clockwise; rotated back it is w x h.
TEST(spine_crop_rotated_region) {
    cotl::SpineRegion r;
    r.rotate = true;
    r.x = 0;
    r.y = 0;
    r.w = 3; // unrotated 3 wide, 2 high, so 2 wide, 3 high in the atlas
    r.h = 2;
    const auto c = cotl::CropRegion(Grid(4, 3), r);
    REQUIRE(c.has_value());
    CHECK_EQ(c->width, 3u);
    CHECK_EQ(c->height, 2u);
    // The original's top left is at the bottom left in the atlas
    CHECK_EQ(Red(*c, 0, 0), uint8_t{20});
    CHECK_EQ(Red(*c, 2, 0), uint8_t{0});
    CHECK_EQ(Red(*c, 0, 1), uint8_t{21});
}

TEST(spine_crop_outside_page_fails) {
    cotl::SpineRegion r;
    r.x = 3;
    r.y = 0;
    r.w = 2;
    r.h = 2;
    CHECK(!cotl::CropRegion(Grid(4, 3), r).has_value());
}

namespace {

// Atlas with full and half red heart and empty container, all from an 8x8 page
cotl::SpineAtlas HeartAtlas() {
    cotl::SpineAtlas a;
    a.page = "heart.png";
    auto region = [](int x, int w, int h, int orig_w, int orig_h) {
        cotl::SpineRegion r;
        r.x = x;
        r.w = w;
        r.h = h;
        r.orig_w = orig_w;
        r.orig_h = orig_h;
        return r;
    };
    a.regions["images/HeartFull"] = region(0, 4, 4, 4, 4);
    a.regions["images/HeartHalfLeft"] = region(4, 2, 4, 2, 4);
    a.regions["images/HeartBack"] = region(6, 2, 2, 2, 2);
    return a;
}

ce_unity::Image Solid(uint32_t w, uint32_t h) {
    ce_unity::Image img;
    img.width = w;
    img.height = h;
    img.rgba.assign(w * h * 4, 255);
    return img;
}

int Opaque(const ce_unity::Image& img) {
    int n = 0;
    for (size_t i = 3; i < img.rgba.size(); i += 4) n += img.rgba[i] == 255;
    return n;
}

} // namespace

TEST(spine_heart_full_centered_on_canvas) {
    const auto h = cotl::ComposeHeart(HeartAtlas(), Solid(8, 8), "red", "full");
    REQUIRE(h.has_value());
    CHECK_EQ(h->width, uint32_t(cotl::kHeartCanvas));
    CHECK_EQ(Opaque(*h), 16);
    const int c = cotl::kHeartCanvas / 2;
    CHECK_EQ(h->rgba[((c - 2) * h->width + c - 2) * 4 + 3], uint8_t{255}); // top left corner
    CHECK_EQ(h->rgba[((c - 3) * h->width + c - 3) * 4 + 3], uint8_t{0});
}

TEST(spine_heart_half_sits_on_left_of_full_frame) {
    const auto h = cotl::ComposeHeart(HeartAtlas(), Solid(8, 8), "red", "half");
    REQUIRE(h.has_value());
    const int c = cotl::kHeartCanvas / 2;
    // half on the left (x c-2..c-1), empty container below in the middle
    CHECK_EQ(h->rgba[((c - 2) * h->width + c - 2) * 4 + 3], uint8_t{255});
    CHECK_EQ(h->rgba[((c - 2) * h->width + c + 1) * 4 + 3], uint8_t{0});
}

TEST(spine_heart_unknown_kind) {
    CHECK(!cotl::ComposeHeart(HeartAtlas(), Solid(8, 8), "gold", "full").has_value());
    CHECK(!cotl::ComposeHeart(HeartAtlas(), Solid(8, 8), "red", "quarter").has_value());
}

TEST(spine_atlas_parses_page_size) {
    const auto a = cotl::ParseSpineAtlas(kAtlas);
    CHECK_EQ(a.page_w, 1024);
    CHECK_EQ(a.page_h, 256);
}

// The Switch texture can be smaller than the atlas page (WeaponCard: 8192 to 4096): region scaled
TEST(spine_crop_scaled_page) {
    cotl::SpineRegion r;
    r.x = 2;
    r.y = 2;
    r.w = 4;
    r.h = 2;
    const auto c = cotl::CropRegion(Grid(4, 3), r, 0.5f); // atlas page 8×6, texture 4×3
    REQUIRE(c.has_value());
    CHECK_EQ(c->width, 2u);
    CHECK_EQ(c->height, 1u);
    CHECK_EQ(Red(*c, 0, 0), uint8_t{11});
    CHECK_EQ(Red(*c, 1, 0), uint8_t{12});
}
