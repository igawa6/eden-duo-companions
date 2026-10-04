// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// dsmod-dread-mapgen-test [romfs_dir] [manifest.json] [out_dir]
// Runs the map generator over a local romfs directory with the package manifest's template and
// writes out_dir/areas.json + out_dir/map/<area>[.<layer>].geo, for comparison with the 1.0.0
// package's shipped data. Without out_dir it only checks that generation succeeds; without a
// manifest (argv[2]) it is skipped. romfs_dir defaults to ./romfs.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "dread_mapgen.h"

namespace {

dread_romfs::ReadFn LocalReader(std::string root) {
    return [root = std::move(root)](const char* path, uint64_t offset, void* out,
                                    size_t size) -> size_t {
        const std::string full = root + "/" + path;
        std::FILE* f = std::fopen(full.c_str(), "rb");
        if (!f)
            return 0;
        size_t result = 0;
        if (!out) {
            if (fseeko(f, 0, SEEK_END) == 0)
                result = size_t(ftello(f));
        } else if (fseeko(f, off_t(offset), SEEK_SET) == 0) {
            result = std::fread(out, 1, size, f);
        }
        std::fclose(f);
        return result;
    };
}

} // namespace

int main(int argc, char** argv) {
    const std::string romfs = argc > 1 ? argv[1] : "romfs";
    const std::string manifest_path = argc > 2 ? argv[2] : "";
    if (manifest_path.empty()) {
        std::printf("skipped: usage dsmod-dread-mapgen-test romfs_dir manifest.json [out_dir]\n");
        return 0;
    }
    const std::string out_dir = argc > 3 ? argv[3] : "";
    std::ifstream mf(manifest_path);
    if (!mf) {
        std::fprintf(stderr, "no manifest %s\n", manifest_path.c_str());
        return 2;
    }
    const auto manifest = nlohmann::json::parse(mf);
    dread_mapgen::Inputs in;
    in.read = LocalReader(romfs);
    in.template_areas = manifest.at("map").at("areas");
    for (const auto& [name, unused] : manifest.at("map").at("icons").items())
        in.known_icons.insert(name);
    in.raster_px = manifest.at("map").at("style").value("raster_px", 3072);
    const int rounds = std::getenv("MAPGEN_ROUNDS") ? std::atoi(std::getenv("MAPGEN_ROUNDS")) : 1;
    dread_mapgen::Output out;
    for (int r = 0; r < rounds; ++r) {
        if (!dread_mapgen::Generate(in, out)) {
            std::fprintf(stderr, "generate failed: %s\n", out.error.c_str());
            return 1;
        }
        std::printf("round %d: %.1f ms, %zu romfs bytes, peak %zu nodes\n", r, out.ms,
                    out.romfs_bytes, out.peak_nodes);
    }
    for (const auto& n : out.notes)
        std::printf("  %s\n", n.c_str());
    size_t blob_bytes = 0;
    for (const auto& [k, b] : out.blobs)
        blob_bytes += b.size();
    const std::string dumped = out.areas.dump();
    std::printf("areas json %zu bytes, %zu blobs %zu bytes\n", dumped.size(), out.blobs.size(),
                blob_bytes);
    if (!out_dir.empty()) {
        std::filesystem::create_directories(out_dir + "/map");
        std::ofstream(out_dir + "/areas.json") << dumped;
        for (const auto& [key, bytes] : out.blobs) {
            std::ofstream f(out_dir + "/" + key, std::ios::binary);
            f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        }
    }
    return 0;
}
