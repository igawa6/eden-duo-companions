// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Finds SpriteAtlas objects in all Addressables bundles (for native/modules/cotl_atlas_bundles_1_5_27.inc).
// Build: g++ -std=c++20 -O2 -I native/modules -I $EDEN/src tools/cotl/bundlescan.cpp -L<build dir> -ldsmod-ce-unity-lib
#include <cstdio>
#include <filesystem>
#include "chained_echoes_unity.h"

int main(int, char** argv) {
    const std::string root = argv[1]; // .../Data/StreamingAssets/aa/Switch
    auto r = ce_unity::MakeDirectoryRangeReader(root);
    size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator(root)) {
        const std::string name = e.path().filename().string();
        if (!name.ends_with(".bundle")) continue;
        ++n;
        ce_unity::ByteSource s;
        s.size = *r.size(name);
        s.read = [r, name](uint64_t o, uint8_t* out, size_t k) { return r.read(name, o, out, k); };
        auto b = ce_unity::Bundle::Open(s);
        if (!b) continue;
        for (const auto& node : b->Nodes()) {
            if (node.path.ends_with(".resS") || node.path.ends_with(".resource")) continue;
            auto f = ce_unity::SerializedFile::Open(b->NodeSource(node));
            if (!f) continue;
            for (const auto& o : f->Objects()) {
                if (o.class_id == 687078895) {
                    std::printf("%s atlas=%s\n", name.c_str(), f->PeekName(o).value_or("?").c_str());
                    std::fflush(stdout);
                }
            }
        }
    }
    std::printf("scanned %zu bundles\n", n);
}
