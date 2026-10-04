// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// TextWidth (acnh_publish.h): the runtime's Canvas::MeasureText(text, 10) for the package font
// "ui:40" without building the atlas: the glyph advances come from the core's Fonts
// (acnh_font.h UiAdvance: the same faces, the same advance rounding as the atlas); code points
// outside the atlas run advance half the line (MeasureText's fallback for a missing glyph).

#include <map>
#include <mutex>

#include "acnh_font.h"
#include "acnh_msbt.h"
#include "acnh_publish.h"

namespace acnh {
namespace {
constexpr uint32_t Cap = 40;           // the package font spec "ui:40"
constexpr float Wanted = 10.0f * 5.0f; // text_scale 10 -> 50 px line
} // namespace

struct TextWidth::Impl {
    Impl(Romfs* romfs, Fonts* shared)
        : own{shared ? nullptr : std::make_unique<Fonts>(*romfs)}, fonts{shared ? *shared : *own} {}
    std::unique_ptr<Fonts> own; ///< only without the module's
    Fonts& fonts;
    std::mutex mutex;
    bool tried = false, ok = false;
    std::map<uint32_t, int> advance; ///< atlas advance in atlas px (-1 = not in the atlas)

    int Advance(uint32_t cp) {
        if (const auto it = advance.find(cp); it != advance.end())
            return it->second;
        const int a = fonts.UiAdvance(Cap, cp).value_or(-1);
        advance.emplace(cp, a);
        return a;
    }
};

TextWidth::TextWidth(Romfs* romfs, Fonts* fonts)
    : impl{romfs || fonts ? std::make_unique<Impl>(romfs, fonts) : nullptr} {}
TextWidth::~TextWidth() = default;

int TextWidth::Measure(const std::string& text) {
    std::unique_lock<std::mutex> lock;
    bool ok = false;
    if (impl) {
        lock = std::unique_lock{impl->mutex};
        if (!impl->tried) {
            impl->tried = true;
            impl->ok = impl->fonts.UiAdvance(Cap, ' ').has_value();
        }
        ok = impl->ok;
    }
    float pen = 0;
    const float ratio = Wanted / static_cast<float>(Cap);
    for (size_t i = 0; i < text.size();) {
        const uint32_t cp = NextCodepoint(text, i);
        if (cp == '\n')
            continue;
        int a = ok ? impl->Advance(cp) : -1;
        // The atlas is one dense run U+0020..U+E2FF: a slot the face does not render advances
        // like a space; only code points outside the run are missing glyphs (half a line).
        if (a < 0 && ok && cp >= 0x20 && cp <= 0xE2FF)
            a = impl->Advance(' ');
        pen += a < 0 ? Wanted * 0.5f : static_cast<float>(a) * ratio;
    }
    return static_cast<int>(pen);
}

} // namespace acnh
