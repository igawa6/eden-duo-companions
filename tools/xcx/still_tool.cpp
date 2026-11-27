// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Offline driver for the Skell still: renders one frame row from a merged RomFS to a PNG and prints
// timing/memory. Private inputs are supplied by the caller; nothing is stored in the repository.
#include "xcx_still.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs = std::filesystem;
namespace {
std::uint32_t Crc(const std::uint8_t *p, std::size_t n, std::uint32_t crc = 0) {
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i)
        crc = table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}
void Put32(std::vector<std::uint8_t> &v, std::uint32_t x) {
    for (int s = 24; s >= 0; s -= 8)
        v.push_back(std::uint8_t(x >> s));
}
void Chunk(std::ofstream &f, const char *type, const std::vector<std::uint8_t> &data) {
    std::vector<std::uint8_t> head;
    Put32(head, std::uint32_t(data.size()));
    f.write(reinterpret_cast<const char *>(head.data()), 4);
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    f.write(reinterpret_cast<const char *>(body.data()), std::streamsize(body.size()));
    std::vector<std::uint8_t> crc;
    Put32(crc, Crc(body.data(), body.size()));
    f.write(reinterpret_cast<const char *>(crc.data()), 4);
}
// PNG with stored (uncompressed) deflate blocks: no zlib needed, large but simple.
void WritePng(const fs::path &path, const xcx::Image &image) {
    std::ofstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot write png");
    const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    f.write(reinterpret_cast<const char *>(sig), 8);
    std::vector<std::uint8_t> ihdr;
    Put32(ihdr, image.w);
    Put32(ihdr, image.h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    Chunk(f, "IHDR", ihdr);
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < image.h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), image.rgba.begin() + std::size_t(y) * image.w * 4,
                   image.rgba.begin() + std::size_t(y + 1) * image.w * 4);
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (auto c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    for (std::size_t pos = 0; pos < raw.size(); pos += 65535) {
        const auto n = std::min<std::size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(std::uint8_t(n & 255));
        z.push_back(std::uint8_t(n >> 8));
        z.push_back(std::uint8_t(~n & 255));
        z.push_back(std::uint8_t((~n >> 8) & 255));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    }
    Put32(z, b << 16 | a);
    Chunk(f, "IDAT", z);
    Chunk(f, "IEND", {});
}
} // namespace
long StatusKb(const char *key) {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind(key, 0) == 0)
            return std::atol(line.c_str() + std::strlen(key));
    return -1;
}
int main(int argc, char **argv) try {
    if (argc < 4) {
        std::cerr << "usage: xcx-still-tool MERGED-ROMFS FRAME-ROW OUT.png [--size WxH] [--no-fold] "
                     "[--ss N] [--record 0x400.bin --player-paint] [--fit-thumb WxH] "
                     "[--palette-name NAMEID]\n";
        return 2;
    }
    const fs::path romfs = fs::canonical(argv[1]);
    xcx::StillOptions options;
    xcx::Bytes record;
    bool player_paint = false;
    int tw = 0, th = 0, palette_name = -1;
    char hybrid = 0;
    bool with_weapons = false;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc)
            std::sscanf(argv[++i], "%dx%d", &options.width, &options.height);
        else if (a == "--no-fold")
            options.fold_pods = false;
        else if (a == "--bind") // the old bind pose (+ pod fold) instead of the field idle
            options.pose_idle = false;
        else if (a == "--ss" && i + 1 < argc)
            options.supersample = std::atoi(argv[++i]);
        else if (a == "--record" && i + 1 < argc) {
            std::ifstream f(argv[++i], std::ios::binary);
            record.assign(std::istreambuf_iterator<char>(f), {});
        } else if (a == "--player-paint")
            player_paint = true;
        else if (a == "--palette-name" && i + 1 < argc)
            palette_name = std::atoi(argv[++i]);
        else if (a == "--yaw" && i + 1 < argc)
            options.yaw_degrees = float(std::atof(argv[++i]));
        else if (a == "--pitch" && i + 1 < argc)
            options.pitch_degrees = float(std::atof(argv[++i]));
        else if (a == "--weapons")
            with_weapons = true;
        else if (a == "--weapons-only")
            options.weapons_only = with_weapons = true;
        else if (a == "--hybrid-a")
            hybrid = 'a';
        else if (a == "--debug-palette")
            options.palette = xcx::StillPalette{{{{255, 0, 0}, {0, 255, 0}, {0, 0, 255},
                                                  {255, 255, 0}, {255, 0, 255}, {0, 255, 255}}}};
        else if (a == "--fit-thumb" && i + 1 < argc)
            std::sscanf(argv[++i], "%dx%d", &tw, &th);
    }
    EdenDsmodHostApi host{};
    host.userdata = const_cast<fs::path *>(&romfs);
    host.read_romfs = [](void *u, const char *path, std::uint64_t offset, void *buffer,
                         std::size_t size) -> std::uint64_t {
        const auto &root = *static_cast<fs::path *>(u);
        fs::path rel = fs::path(path).relative_path();
        for (const auto &part : rel)
            if (part == "..")
                return 0;
        std::ifstream f(root / rel, std::ios::binary);
        if (!f)
            return 0;
        f.seekg(std::streamoff(offset));
        f.read(static_cast<char *>(buffer), std::streamsize(size));
        return std::uint64_t(f.gcount());
    };
    const long rss_before = StatusKb("VmRSS:");
    xcx::Archive archive;
    if (std::string_view(argv[2]).starts_with("/")) { // dump one archive member (decoded) to a file
        const auto bytes = archive.Read(host, argv[2]);
        std::ofstream(argv[3], std::ios::binary)
            .write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
        std::printf("%s %zu bytes\n", argv[2], bytes.size());
        return bytes.empty() ? 1 : 0;
    }
    if (std::string_view(argv[2]).starts_with("wd")) { // --probe: describe a weapon model
        const auto g = xcx::ReadWeaponGeometry(archive, host, argv[2]);
        std::printf("%s meshes %zu materials %zu textures %zu bones %zu\n", argv[2],
                    g.meshes.size(), g.materials.size(), g.texture_sources.size(),
                    g.skin.bones.size());
        for (const auto &m : g.materials) {
            std::printf(" material %s:", m.name.c_str());
            for (const auto &t : m.textures)
                std::printf(" %u(u%u,f%u)", t.texture, g.texture_sources[t.texture].usage, t.sampler_flags);
            std::printf("\n");
        }
        for (const auto &b : g.skin.bones)
            std::printf(" bone %s parent %d pos %.2f %.2f %.2f\n", b.name.c_str(), b.parent,
                        b.bind_position[0], b.bind_position[1], b.bind_position[2]);
        xcx::FrameTextureReader reader(archive);
        for (unsigned t = 0; t < g.texture_sources.size(); ++t) {
            const auto img = reader.Read(host, g, t);
            if (const char *dir = std::getenv("XCX_PROBE_DUMP")) { // each texture as a PAM (RGBA) for viewing
                std::ofstream pam(fs::path(dir) / (std::string(argv[2]) + "-tex" + std::to_string(t) + ".pam"),
                                  std::ios::binary);
                pam << "P7\nWIDTH " << img.w << "\nHEIGHT " << img.h << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
                pam.write(reinterpret_cast<const char *>(img.rgba.data()), std::streamsize(img.rgba.size()));
            }
            std::map<int, int> hist;
            long sum[3] = {0, 0, 0};
            for (std::size_t i = 0; i < std::size_t(img.w) * img.h; ++i) {
                ++hist[img.rgba[i * 4]];
                for (int k = 0; k < 3; ++k)
                    sum[k] += img.rgba[i * 4 + k];
            }
            std::printf(" tex %u usage %u %dx%d mean %.0f %.0f %.0f distinctR %zu:", t,
                        g.texture_sources[t].usage, img.w, img.h, sum[0] / double(img.w * img.h),
                        sum[1] / double(img.w * img.h), sum[2] / double(img.w * img.h), hist.size());
            int shown = 0;
            for (const auto &[v, n] : hist)
                if (n > img.w * img.h / 50 && shown++ < 8)
                    std::printf(" %d(%d%%)", v, int(100.0 * n / (double(img.w) * img.h)));
            std::printf("\n");
        }
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (const auto &vb : g.vertices)
            for (const auto &v : vb)
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], v.position[k]);
                    hi[k] = std::max(hi[k], v.position[k]);
                }
        std::printf(" bbox %.2f %.2f %.2f .. %.2f %.2f %.2f\n", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        return 0;
    }
    const auto frame = xcx::FrameModelForRow(archive, host, unsigned(std::atoi(argv[2])));
    if (player_paint) {
        auto p = xcx::PlayerPaintFromRecord(record);
        if (!p)
            throw std::runtime_error("record paint refused");
        options.palette = *p;
    }
    if (with_weapons) {
        const auto items = xcx::WeaponItemsFromRecord(record);
        std::printf("weapon items:");
        for (auto v : items)
            std::printf(" %u", v);
        const auto loc = xcx::ParseLocators(archive.Read(host, "/chr/dl/" + frame.resource + ".wimtp"));
        std::printf("\nframe locators %zu\n", loc.size());
        const auto mounts = xcx::ResolveWeapons(archive, host, frame, xcx::WeaponItemsFromRecord(record));
        for (const auto &m : mounts)
            std::printf("weapon block %u %s on %s (weapon bone '%s')\n", m.block, m.resource.c_str(),
                        m.skell_bone.c_str(), m.weapon_bone.c_str());
        options.weapons = mounts;
    }
    if (hybrid == 'a' && options.palette) { // record paint on slots 0..3, factory glow on 4..5
        const auto factory = xcx::FactoryPalette(archive, host, frame.name_id);
        options.palette->rgb[4] = factory.rgb[4];
        options.palette->rgb[5] = factory.rgb[5];
    }
    if (palette_name >= 0)
        options.palette = xcx::FactoryPalette(archive, host, unsigned(palette_name));
    xcx::StillStats stats;
    const auto image = xcx::RenderStill(archive, host, frame, options, nullptr, &stats);
    // Peak resident growth of the whole render job: the archive index, geometry, textures and
    // buffers (the process high-water mark minus the resident size before the job started).
    const long hwm_after = StatusKb("VmHWM:");
    std::printf("rss_before_kb %ld hwm_after_kb %ld job_peak_growth_mib %.1f\n", rss_before,
                hwm_after, (hwm_after - rss_before) / 1024.0);
    if (!stats.weapon_notes.empty())
        std::printf("weapon notes: %s\n", stats.weapon_notes.c_str());
    WritePng(argv[3], image);
    if (tw && th)
        WritePng(std::string(argv[3]) + ".thumb.png", xcx::FitStill(image, tw, th));
    std::printf("%s frame %u name %u total_ms %.1f geometry_ms %.1f textures_ms %.1f raster_ms %.1f "
                "shade_ms %.1f tris %zu meshes %zu covered %zu tex_bytes %zu transient %zu "
                "buffers %zu tracked %zu folded %d posed %d glass %d weapons %zu\n",
                frame.resource.c_str(), frame.frame, frame.name_id, stats.ms_total,
                stats.ms_geometry, stats.ms_textures, stats.ms_raster, stats.ms_shade,
                stats.triangles, stats.drawn_meshes, stats.covered_samples, stats.texture_bytes,
                stats.transient_peak, stats.buffer_bytes, stats.tracked_peak_bytes(), stats.folded,
                stats.posed, stats.glass, stats.weapons);
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
