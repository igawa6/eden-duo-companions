#!/usr/bin/env python3
"""Offline ASan/UBSan cache test; no ROMFS, host API, or device access."""
import os
import shlex
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = r'''
#include "pla_asset_cache.h"
#include <atomic>
#include <cassert>
#include <limits>
#include <thread>
#include <vector>
using V = std::vector<unsigned char>;
using Cache = pla::AssetCache<V>;
auto Asset(size_t size, unsigned char value = 0) {
    return std::make_shared<const V>(size, value);
}
int main() {
    Cache c(10, 2);
    assert(!c.Find("missing"));
    assert(!c.Insert("failure", {}, 2));
    assert(!c.Insert("failure", Asset(0), 0));
    assert(!c.Find("failure"));
    auto held = c.Insert("a", Asset(4, 1), 4);
    c.Insert("b", Asset(4, 2), 4);
    assert(c.Bytes() == 8 && c.Size() == 2);
    assert(c.Find("a") == held);
    c.Insert("c", Asset(4, 3), 4);
    assert(!c.Find("b") && c.Find("a") && c.Find("c"));
    c.Insert("d", Asset(10, 4), 10);
    assert(c.Bytes() == 10 && c.Size() == 1 && !c.Find("a"));
    assert((*held)[0] == 1);
    auto oversized = c.Insert("large", Asset(11, 5), 11);
    assert(oversized && !c.Find("large") && c.Bytes() == 10);
    c.Insert("d", Asset(2, 6), 2);
    assert(c.Bytes() == 2 && c.Size() == 1 && (*c.Find("d"))[0] == 6);
    c.Clear();
    assert(c.Bytes() == 0 && c.Size() == 0 && !c.Find("d"));
    assert((*held)[0] == 1);
    Cache slots(100, 1);
    slots.Insert("one", Asset(1), 1); slots.Insert("two", Asset(1), 1);
    assert(!slots.Find("one") && slots.Size() == 1);
    Cache disabled(0);
    assert(disabled.Insert("valid", Asset(1), 1) && !disabled.Find("valid"));
    Cache no_slots(100, 0);
    assert(no_slots.Insert("valid", Asset(1), 1) && !no_slots.Find("valid"));
    assert(c.Insert(std::string(513, 'x'), Asset(1), 1) && c.Size() == 0);
    Cache wide(std::numeric_limits<size_t>::max());
    wide.Insert("big-account", Asset(1), std::numeric_limits<size_t>::max());
    wide.Insert("small", Asset(1), 1);
    assert(wide.Bytes() == 1 && !wide.Find("big-account"));
    // Contending Find/Insert/Clear: handles must stay valid while other threads
    // replace/evict them. Every payload has a deterministic key-specific value.
    Cache concurrent(64, 8);
    std::atomic<bool> start = false;
    std::vector<std::thread> threads;
    for (unsigned worker = 0; worker < 6; ++worker) {
        threads.emplace_back([&, worker] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (unsigned n = 0; n < 3000; ++n) {
                const unsigned id = (n + worker) % 12;
                const auto key = std::to_string(id);
                auto p = concurrent.Find(key);
                if (!p) p = concurrent.Insert(key, Asset(8, id), 8);
                assert(p && p->size() == 8);
                for (auto value : *p) assert(value == id);
                if (worker == 0 && n % 71 == 0) concurrent.Clear();
                assert(concurrent.Bytes() <= 64 && concurrent.Size() <= 8);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto &thread : threads) thread.join();
    concurrent.Clear();
    assert(concurrent.Bytes() == 0 && concurrent.Size() == 0);
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix="pla-asset-cache-") as temp:
        source = Path(temp) / "test.cpp"
        binary = Path(temp) / "test"
        source.write_text(SOURCE)
        subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++20", "-Wall", "-Wextra",
                        "-Werror", "-pthread", "-fsanitize=address,undefined", "-g",
                        "-I", str(ROOT / "native/modules"), str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("PLA asset cache: ASan/UBSan + concurrent access passed")

if __name__ == "__main__":
    main()
