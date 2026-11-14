// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/mods/dsmod_module_extensions.h"
#include <array>
#include <atomic>
#include <vector>
namespace re4::assets {
struct Cache;
struct Image;
struct Font {
    // Worker/decoder calls are serialized by the host loader mutex; Sample reads only ready/installed atomics.
    bool attempted=false;
    unsigned attempts=0;
    uint32_t ascent=0;
    std::vector<uint8_t> packed;
    std::array<EdenDsmodFontGlyph,95> glyphs{};
    std::atomic<bool> ready{false},installed{false};
    void Warm(const EdenDsmodHostApi*,const Image* atlas=nullptr);
    EdenDsmodBool Decode(const uint8_t*,size_t,void*,EdenDsmodFontSink);
};
}
