// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_follower_portrait.h"
#include "cotl_test_util.h"

namespace {

ce_unity::Image Solid(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    ce_unity::Image img;
    img.width = w;
    img.height = h;
    img.rgba.resize(w * h * 4);
    for (size_t i = 0; i < size_t{w} * h; ++i) {
        img.rgba[i * 4] = r;
        img.rgba[i * 4 + 1] = g;
        img.rgba[i * 4 + 2] = b;
        img.rgba[i * 4 + 3] = a;
    }
    return img;
}

const uint8_t* Px(const ce_unity::Image& img, int x, int y) {
    return &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
}

// Region 10×10 (original pixels) without rotation: px → wx = px + tx, py → wy = ty − py
cotl::PortraitPart Part(std::string_view slot, std::string_view region, float tx, float ty) {
    return {slot, region, {1, 0, 0, -1, tx, ty}};
}

} // namespace

TEST(portrait_parts_known_skin) {
    const auto parts = cotl::PortraitParts("Cat");
    REQUIRE(parts.size() == 5u);
    CHECK_EQ(parts[0]->slot, std::string_view("HEAD_SKIN_BTM"));
    CHECK_EQ(parts[0]->region, std::string_view("Head/HeadCat_Btm"));
    CHECK_EQ(parts[4]->slot, std::string_view("EYE_LEFT"));
    CHECK(cotl::PortraitParts("NoSuchSkin").empty());
}

TEST(portrait_arg_round_trip) {
    const std::vector<cotl::SlotTint> tints{{"HEAD_SKIN_TOP", 0xFFAA0080}, {"MARKINGS", 0x102030FF}};
    const std::string arg = cotl::PortraitArg("Boss Spiker", tints);
    CHECK_EQ(arg, std::string("Boss Spiker;HEAD_SKIN_TOP=FFAA0080;MARKINGS=102030FF"));
    const auto spec = cotl::ParsePortraitArg(arg);
    REQUIRE(spec.has_value());
    CHECK_EQ(spec->skin, std::string("Boss Spiker"));
    REQUIRE(spec->tints.size() == 2u);
    CHECK_EQ(spec->tints[1].slot, std::string("MARKINGS"));
    CHECK_EQ(spec->tints[1].rgba, 0x102030FFu);
    CHECK_EQ(cotl::ParsePortraitArg("Mutation/1")->skin, std::string("Mutation/1"));
    CHECK(!cotl::ParsePortraitArg("Cat;HEAD=XYZ").has_value());
    CHECK(!cotl::ParsePortraitArg("").has_value());
}

// Two parts side by side: the image fills the common extent, the right one lies on the right,
// the slot's colour multiplies, the later part lies on top.
TEST(portrait_compose_places_and_tints) {
    const auto white = Solid(10, 10, 255, 255, 255);
    const auto blue = Solid(10, 10, 0, 0, 255);
    const cotl::PortraitPart a = Part("HEAD_SKIN_BTM", "white", 0, 10);
    const cotl::PortraitPart b = Part("EYE_LEFT", "blue", 10, 10);
    const cotl::PortraitPart* parts[] = {&a, &b};
    const std::vector<cotl::SlotTint> tints{{"HEAD_SKIN_BTM", 0xFF0000FF}};
    const auto img = cotl::ComposePortrait(
        parts, [&](std::string_view r) { return r == "white" ? &white : r == "blue" ? &blue : nullptr; },
        1.0f, tints, 40);
    REQUIRE(img.has_value());
    CHECK_EQ(img->width, 40u);
    CHECK_EQ(img->height, 40u);
    const uint8_t* left = Px(*img, 10, 20);
    CHECK_EQ(int{left[0]}, 255);
    CHECK_EQ(int{left[1]}, 0); // white × red
    CHECK_EQ(int{left[3]}, 255);
    const uint8_t* right = Px(*img, 30, 20);
    CHECK_EQ(int{right[2]}, 255);
    CHECK_EQ(int{right[0]}, 0);
    // 20 × 10 world onto 40² centred: top and bottom stay transparent
    CHECK_EQ(int{Px(*img, 20, 2)[3]}, 0);
    CHECK_EQ(int{Px(*img, 20, 37)[3]}, 0);
}

// Image at half resolution (scale 0.5) covers the same world area
TEST(portrait_compose_scaled_regions) {
    const auto half = Solid(5, 5, 0, 255, 0);
    const cotl::PortraitPart a = Part("HEAD_SKIN_BTM", "g", 0, 10);
    const cotl::PortraitPart* parts[] = {&a};
    const auto img = cotl::ComposePortrait(parts, [&](std::string_view) { return &half; }, 0.5f, {}, 20);
    REQUIRE(img.has_value());
    CHECK_EQ(int{Px(*img, 10, 10)[1]}, 255);
    CHECK_EQ(int{Px(*img, 10, 10)[3]}, 255);
}

TEST(portrait_compose_without_images_fails) {
    const cotl::PortraitPart a = Part("HEAD_SKIN_BTM", "missing", 0, 10);
    const cotl::PortraitPart* parts[] = {&a};
    CHECK(!cotl::ComposePortrait(parts, [](std::string_view) { return nullptr; }, 1.0f, {}).has_value());
}

// Keys only for slots the skin draws, without white; empty for an unknown skin
TEST(portrait_key_filters_slots) {
    const std::vector<cotl::SlotTint> tints{
        {"HEAD_SKIN_TOP", 0xFF8000FF}, {"ARM_LEFT", 0x123456FF}, {"HEAD_SKIN_BTM", 0xFFFFFFFF}};
    CHECK_EQ(cotl::PortraitKey("Cat", tints), std::string("module:cotl:follower/Cat;HEAD_SKIN_TOP=FF8000FF"));
    CHECK_EQ(cotl::PortraitKey("NoSuchSkin", tints), std::string());
    CHECK_EQ(cotl::PortraitKey("", tints), std::string());
}
