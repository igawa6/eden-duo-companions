// SPDX-License-Identifier: GPL-3.0-or-later
// Test fixture (dev.33): the native item-flag path the module's sword reader takes (sshd_dpad.h
// ReadSword) over a test's slot item bytes. File manager *(main+0x2BF9CC0) + 0x10 = the slot (file
// A: an empty registry), item manager *(main+0x182E168) -> +0x18 -> +8 = a metadata table whose sword
// records (flag f: word, shift << 4) point at the slot bits the earlier fixtures already set:
// 11 Goddess +0x9E5:0, 12 Longsword +0x9E5:1, 13 Master +0x9E5:2, 9 White +0x9FA:4, 14 True Master
// +0x9F2:7 and 10 Practice +0x9F3:0 (the decoded (7, 0x80)).
#pragma once

#include <cstdint>
#include <cstring>

namespace sshd_test {

constexpr uint64_t kFixSlot = 0x5AEAD54, kFixItemMgr = 0x7200000;

// Answers the reads of that path relative to `main`; `items` holds slot+0x9E0..+0x9FF.
inline bool SwordFixtureRead(uint64_t main, const uint8_t* items, uint64_t a, void* out, size_t n) {
    const auto put64 = [&](uint64_t v) {
        if (n != 8) return false;
        std::memcpy(out, &v, 8);
        return true;
    };
    if (a == main + 0x2BF9CC0) return put64(main + kFixSlot - 0x10);
    if (a == main + 0x1688510) return put64(0); // registry: no type-1 object -> file A
    if (a == main + 0x182E168) return put64(kFixItemMgr);
    if (a == kFixItemMgr + 0x18) return put64(kFixItemMgr + 0x100);
    if (a == kFixItemMgr + 0x100 + 8) return put64(kFixItemMgr + 0x200);
    if (a >= kFixItemMgr + 0x200 && a + n <= kFixItemMgr + 0x200 + 2 * 16 && n == 2) {
        static constexpr uint8_t kMeta[16][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
                                                 {11, 0x40}, {7, 0x80}, {0, 0x80}, {0, 0x90}, {0, 0xA0}, {7, 0x70}, {0, 0}};
        std::memcpy(out, kMeta[(a - kFixItemMgr - 0x200) / 2], 2);
        return true;
    }
    const uint64_t words = main + kFixSlot + 0x9E4;
    if (a >= words && a + n <= main + kFixSlot + 0xA00) {
        std::memcpy(out, items + (a - main - kFixSlot - 0x9E0), n);
        return true;
    }
    return false;
}

} // namespace sshd_test
