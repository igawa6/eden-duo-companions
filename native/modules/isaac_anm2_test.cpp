// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "isaac_anm2.h"
#include "isaac_map.h"

#include <cassert>
#include <cstring>
#include <iostream>

namespace {
struct Bundle {
    std::vector<std::uint8_t> bytes;
    template <class T> void Put(T value) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(value));
    }
    void Text(std::string_view s) {
        Put<std::uint16_t>(s.size());
        bytes.insert(bytes.end(), s.begin(), s.end());
    }
};
} // namespace
int main() {
    Bundle b;
    b.Put<std::uint32_t>(1);
    b.Put(isaac_anm2::BundleHash("gfx/ui/minimap2.anm2"));
    b.Text("gfx/ui");
    b.Put<std::uint32_t>(1);
    b.Put<std::uint32_t>(0);
    b.Text("minimap2.png"); // sheets
    b.Put<std::uint32_t>(1);
    b.Put<std::uint32_t>(0);
    b.Put<std::uint32_t>(0);
    b.Text("Room"); // layers
    b.Put<std::uint32_t>(0);
    b.Put<std::uint32_t>(0); // nulls, events
    b.Text("RoomVisited");
    b.Put<std::uint32_t>(1);
    b.Text("RoomVisited");
    b.Put<std::uint32_t>(1);
    b.Put<std::uint8_t>(0);
    b.Put<std::uint32_t>(0); // loop, root frames
    b.Put<std::uint32_t>(1);
    b.Put<std::uint32_t>(0);
    b.Put<std::uint8_t>(1);
    b.Put<std::uint32_t>(1); // layer frames
    // crop, position, scale, pivot: all coordinate pairs deliberately differ.
    for (float v : {108.f, 48.f, 18.f, 16.f, 2.f, 3.f, 1.f, 1.f, -4.f, -4.f})
        b.Put(v);
    b.Put<std::int32_t>(1);
    b.Put<std::uint8_t>(1);
    for (int i = 0; i < 8; ++i)
        b.Put(0.f);
    b.Put<std::uint8_t>(0); // color, rotation, interpolation
    b.Put<std::uint32_t>(0);
    b.Put<std::uint32_t>(0); // null animations, events
    std::unordered_map<std::uint32_t, std::shared_ptr<const isaac_anm2::File>> files;
    assert(isaac_anm2::ParseBundle(b.bytes, files));
    const auto& f = files.at(isaac_anm2::BundleHash("gfx/ui/minimap2.anm2"));
    const auto& fr = f->Find("RoomVisited")->layers[0].frames[0];
    assert(fr.pivot_x == -4 && fr.pivot_y == -4);
    assert(fr.pos_x == 2 && fr.pos_y == 3);
    assert(fr.x == 108 && fr.y == 48 && fr.w == 18 && fr.h == 16);
    assert(fr.scale_x == 100 && fr.scale_y == 100);
    // AB+ fill crop is inset by four pixels inside the 26x24 opaque outline.
    isaac_map::Sprites sp;
    sp.outline = {isaac_pcx::Blank(26, 24), 0, 0, true};
    auto fill = isaac_pcx::Blank(18, 16);
    for (size_t i = 0; i < fill.px.size(); i += 4) {
        fill.px[i] = 255;
        fill.px[i + 3] = 255;
    }
    sp.room[2][0] = {fill, fr.pivot_x, fr.pivot_y, true};
    isaac_map::Floor floor;
    floor.rooms.push_back({0, 1, 1, 1, 0, 1});
    auto canvas = isaac_map::RenderCanvas(floor, sp);
    const auto layout = isaac_map::PlanLayout(floor);
    const auto k = layout.k;
    assert(canvas.At(layout.ox + 3 * k, layout.oy + 4 * k)[3] == 0);
    assert(canvas.At(layout.ox + 4 * k, layout.oy + 4 * k)[3] == 255);
    assert(canvas.At(layout.ox + 22 * k, layout.oy + 4 * k)[3] == 0);
    b.bytes.pop_back();
    assert(!isaac_anm2::ParseBundle(b.bytes, files));
    std::cout << "compiled frame coordinates, map inset and truncated bundle passed\n";
}
