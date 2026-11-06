// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Minimal test framework and a fake guest memory for the module tests.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "cotl_guest.h"

namespace test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& Registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& Failures() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

} // namespace test

#define TEST(name)                                                                                 \
    static void name();                                                                            \
    static test::Registrar registrar_##name(#name, name);                                          \
    static void name()

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                          \
            ++test::Failures();                                                                    \
        }                                                                                          \
    } while (0)

// Aborts the test if the condition does not hold (e.g. before accessing an optional).
#define REQUIRE(cond)                                                                              \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                          \
            ++test::Failures();                                                                    \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        const auto va_ = (a);                                                                      \
        const auto vb_ = (b);                                                                      \
        if (!(va_ == vb_)) {                                                                       \
            std::printf("  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);                   \
            ++test::Failures();                                                                    \
        }                                                                                          \
    } while (0)

// Guest memory made of single ranges. A read only succeeds if it lies entirely in
// one range, like is_mapped + read_memory on the real host.
class FakeGuest : public cotl::Guest {
public:
    bool Read(uint64_t addr, void* out, size_t n) const override {
        if (on_read) {
            on_read(addr);
        }
        if (addr == unreadable) {
            return false;
        }
        for (const auto& [base, bytes] : regions_) {
            if (addr >= base && addr + n <= base + bytes.size()) {
                std::memcpy(out, bytes.data() + (addr - base), n);
                return true;
            }
        }
        return false;
    }

    uint64_t unreadable = 0; // reading exactly this address fails

    void Put(uint64_t addr, const void* data, size_t n) {
        auto& region = Region(addr, n);
        std::memcpy(region.second.data() + (addr - region.first), data, n);
    }
    void PutU64(uint64_t addr, uint64_t v) { Put(addr, &v, 8); }
    void PutU32(uint64_t addr, uint32_t v) { Put(addr, &v, 4); }
    void PutS32(uint64_t addr, int32_t v) { Put(addr, &v, 4); }
    void PutF32(uint64_t addr, float v) { Put(addr, &v, 4); }
    void PutU8(uint64_t addr, uint8_t v) { Put(addr, &v, 1); }

    // System.String: length (s32) @0x10, UTF-16 from 0x14. ASCII test texts only.
    void PutString(uint64_t obj, const std::string& ascii) {
        PutS32(obj + 0x10, static_cast<int32_t>(ascii.size()));
        for (size_t i = 0; i < ascii.size(); ++i) {
            const uint16_t c = static_cast<unsigned char>(ascii[i]);
            Put(obj + 0x14 + i * 2, &c, 2);
        }
    }

    // List<T> (object @list) with an array (@array) for references: _items @0x10, _size @0x18,
    // array elements from 0x20.
    void PutRefList(uint64_t list, uint64_t array, const std::vector<uint64_t>& items) {
        PutU64(list + 0x10, array);
        PutS32(list + 0x18, static_cast<int32_t>(items.size()));
        PutU64(array + 0x18, items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            PutU64(array + 0x20 + i * 8, items[i]);
        }
    }
    void PutIntList(uint64_t list, uint64_t array, const std::vector<int32_t>& items) {
        PutU64(list + 0x10, array);
        PutS32(list + 0x18, static_cast<int32_t>(items.size()));
        PutU64(array + 0x18, items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            PutS32(array + 0x20 + i * 4, items[i]);
        }
    }

    std::function<void(uint64_t)> on_read;

private:
    std::pair<const uint64_t, std::vector<uint8_t>>& Region(uint64_t addr, size_t n) {
        constexpr uint64_t kPage = 0x10000;
        const uint64_t base = addr & ~(kPage - 1);
        auto& bytes = regions_[base];
        const size_t need = static_cast<size_t>(addr + n - base);
        if (bytes.size() < need) {
            bytes.resize((need + kPage - 1) & ~(kPage - 1));
        }
        return *regions_.find(base);
    }

    std::map<uint64_t, std::vector<uint8_t>> regions_;
};
