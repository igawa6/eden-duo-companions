// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// fe3h_romfs: LinkData patch overrides that name loose files. Synthetic romfs; no game data.

#include "fe3h_romfs.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

void Put64(Bytes& b, size_t at, uint64_t v) {
    std::memcpy(b.data() + at, &v, 8);
}

// One INFO0 record (0x120): id, plain size, stored size, compressed flag, "rom:/" path.
void Info0(Bytes& b, uint64_t id, uint64_t size, uint64_t stored, uint64_t flag, const char* path) {
    const size_t at = b.size();
    b.resize(at + 0x120);
    Put64(b, at, id);
    Put64(b, at + 8, size);
    Put64(b, at + 16, stored);
    Put64(b, at + 24, flag);
    std::memcpy(b.data() + at + 0x20, path, std::strlen(path));
}

Bytes Pattern(size_t n, uint8_t seed) {
    Bytes b(n);
    for (size_t i = 0; i < n; ++i)
        b[i] = static_cast<uint8_t>(seed + i * 7);
    return b;
}

} // namespace

int main() {
    std::map<std::string, Bytes> fs;
    // DATA0: four empty slots; DATA1 non-empty so the archive opens
    fs["DATA0.bin"] = Bytes(4 * 0x20, 0);
    fs["DATA1.bin"] = Bytes(16, 0);
    // patch4: id 0 exact size, id 1 overstated (plain), id 2 overstated (compressed), id 3 missing
    Bytes info0;
    Info0(info0, 0, 40, 40, 0, "rom:/mods/exact.bin");
    Info0(info0, 1, 100, 100, 0, "rom:/mods/Misc/6108 - Character_Sheet.g1t");
    Info0(info0, 2, 200, 100, 1, "rom:/mods/packed.bin");
    Info0(info0, 3, 10, 10, 0, "rom:/mods/missing.bin");
    fs["patch4/INFO0.bin"] = info0;
    Bytes info2(16, 0);
    Put64(info2, 0, 4);
    fs["patch4/INFO2.bin"] = info2;
    fs["mods/exact.bin"] = Pattern(40, 1);
    fs["mods/Misc/6108 - Character_Sheet.g1t"] = Pattern(60, 2);
    fs["mods/packed.bin"] = Pattern(60, 3);

    fe3h_romfs::LinkData link([&](const char* path, uint64_t offset, void* out, size_t size) -> size_t {
        const auto it = fs.find(path);
        if (it == fs.end())
            return 0;
        if (!out)
            return it->second.size();
        if (offset >= it->second.size())
            return 0;
        const size_t n = std::min<size_t>(size, it->second.size() - offset);
        std::memcpy(out, it->second.data() + offset, n);
        return n;
    });
    assert(link.Open());
    assert(link.PatchDir() == "patch4");
    assert(link.PatchedCount() == 4);

    Bytes got;
    const auto exact = link.Entry(0);
    assert(exact && exact->Size() == 40 && exact->ReadAll(got) && got == fs["mods/exact.bin"]);

    // a plain loose file shorter than its record is read at its own length
    const auto sheet = link.Entry(1);
    assert(sheet && sheet->Size() == 60);
    assert(sheet->ReadAll(got) && got == fs["mods/Misc/6108 - Character_Sheet.g1t"]);

    // a compressed entry still needs every stored byte; a missing file still fails
    assert(!link.Entry(2));
    assert(!link.Entry(3));

    std::puts("fe3h romfs: ok");
    return 0;
}
