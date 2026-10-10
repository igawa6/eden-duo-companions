// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lp_unity.h"
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace lp_poketch {

// Keep native640x480 detail. The owner's square grid is decoration rather than
// framebuffer resolution: never average art into8x8 blocks. Art and grid share
// source coordinates and point scaling, so the grid cannot drift during resize.
inline lp_unity::Image Display(const std::vector<std::uint8_t>& grey,
                               const std::array<std::uint8_t, 3>& colour, int scale) {
    constexpr int width = 640, height = 480;
    const int ow = width * scale / 1000, oh = height * scale / 1000;
    lp_unity::Image out{static_cast<std::uint32_t>(ow), static_cast<std::uint32_t>(oh),
                        std::vector<std::uint8_t>(static_cast<std::size_t>(ow) * oh * 4)};
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            const int sx = x * 1000 / scale, sy = y * 1000 / scale;
            const int value = grey[sy * width + sx];
            // One native pixel per8px tile,6% darker than the surrounding LCD.
            const int grid = sx % 8 == 0 || sy % 8 == 0 ? 240 : 255;
            auto* px = out.rgba.data() + (static_cast<std::size_t>(y) * ow + x) * 4;
            for (int ch = 0; ch < 3; ++ch) {
                px[ch] = static_cast<std::uint8_t>(value * grid * colour[ch] / (255 * 255));
            }
            px[3] = 255;
        }
    return out;
}

// Runtime 18 decodes module pictures asynchronously. Retain one successful picture
// beneath its replacement, so a cache miss cannot expose the casing/background.
// All LCD pictures are opaque. Metadata only, bounded; never retains game pixels.
class Frames {
public:
    using Clock = std::chrono::steady_clock;
    void Inactive(int channel) {
        std::scoped_lock lock{mutex};
        channels.at(channel).active = false;
    }
    void Ready(const std::string& key, Clock::time_point now = Clock::now()) {
        std::scoped_lock lock{mutex};
        decoded[key] = now;
        while (decoded.size() > 64) {
            auto oldest = std::min_element(decoded.begin(), decoded.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
            decoded.erase(oldest);
        }
    }
    std::string Fallback(int channel, const std::string& key, const std::string& base,
                         Clock::time_point now = Clock::now()) {
        std::scoped_lock lock{mutex};
        auto& frame = channels.at(channel);
        if (!frame.active) {
            // On fullscreen entry, the boxed backup is still being drawn and
            // kept hot by the runtime. An old fullscreen key may be evicted.
            if (channel >= 2) {
                const auto& boxed = channels.at(channel - 2);
                frame.last = !boxed.last.empty() ? boxed.last :
                    decoded.contains(base) ? base : std::string{};
            }
        }
        if (!frame.active || frame.requested != key) {
            frame.requested = key;
            frame.since = now;
            frame.active = true;
        }
        // The sink completed on the image worker; leave two tick intervals for
        // the runtime to drain its completed queue before retiring the old frame.
        if (const auto ready = decoded.find(key);
            ready != decoded.end() && ready->second >= frame.since &&
            now - ready->second >= std::chrono::milliseconds{100}) {
            frame.last = key;
            return {};
        }
        // A failed replacement must not leave a permanently misleading old app.
        if (now - frame.since >= std::chrono::seconds{3}) return {};
        // Historical Ready metadata cannot prove a runtime cache hit. Keep the
        // last confirmed backup beneath a revisited key until a fresh decode;
        // if the key is cached its opaque image simply covers that backup.
        if (!frame.last.empty()) return frame.last;
        // Fullscreen can immediately enlarge the already cached boxed LCD.
        return decoded.contains(base) ? base : std::string{};
    }
private:
    struct Frame { std::string requested, last; Clock::time_point since{}; bool active{}; };
    std::mutex mutex;
    std::map<std::string, Clock::time_point> decoded;
    std::array<Frame, 4> channels{};
};
} // namespace lp_poketch
