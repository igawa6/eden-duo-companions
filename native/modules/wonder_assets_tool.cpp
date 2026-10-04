// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Host-side helper for the Wonder companion's asset decoder, run against a romfs dump:
//   wonder-assets-tool <romfs> list <path>        SARC members, or BNTX texture names
//   wonder-assets-tool <romfs> dump <key> <out>   decode a module:wonder:<key> image to PAM (RGBA)
// It exercises exactly the code the module ships.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "wonder_assets.h"

using namespace WonderAssets;

static std::optional<std::vector<std::uint8_t>> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <romfs> list <path> | dump <key> <out.pam>\n", argv[0]);
        return 2;
    }
    const std::string root = argv[1], op = argv[2];
    const RomfsReader read = [&](const std::string& p) { return ReadFile(root + p); };
    if (op == "list") {
        const auto raw = read(argv[3]);
        if (!raw)
            return 1;
        const auto bytes = Zstd(*raw);
        if (!bytes)
            return 1;
        for (const auto& n : SarcNames(*bytes)) {
            std::printf("member %s\n", n.c_str());
            if (n.ends_with(".bntx"))
                for (const auto& t : BntxNames(*SarcMember(*bytes, n)))
                    std::printf("  tex %s\n", t.c_str());
        }
        for (const auto& t : BntxNames(*bytes))
            std::printf("tex %s\n", t.c_str());
        return 0;
    }
    if (op == "dump" && argc >= 5) {
        Decoder decoder;
        const auto image = decoder.Load(read, argv[3]);
        if (!image) {
            std::fprintf(stderr, "decode failed: %s\n", argv[3]);
            return 1;
        }
        std::ofstream o(argv[4], std::ios::binary);
        o << "P7\nWIDTH " << image->width << "\nHEIGHT " << image->height
          << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
        o.write(reinterpret_cast<const char*>(image->rgba.data()),
                static_cast<std::streamsize>(image->rgba.size()));
        std::printf("%s %ux%u\n", argv[3], image->width, image->height);
        return 0;
    }
    return 2;
}
