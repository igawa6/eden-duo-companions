// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Native archive/geometry preparation. Private inputs and outputs are supplied separately.
#include "xcx_model.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
namespace fs = std::filesystem;
using Json = nlohmann::json;
xcx::Bytes Read(const fs::path &p) {
    if (fs::file_size(p) != 0x400)
        throw std::runtime_error("Expected exact owned record");
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot read owned record");
    return {(std::istreambuf_iterator<char>(f)), {}};
}
template <class F> void Refuses(F &&f, const char *why) {
    try {
        f();
    } catch (const std::runtime_error &) {
        return;
    }
    throw std::runtime_error(why);
}
void Put(xcx::Bytes &b, std::size_t p, std::uint32_t v) {
    if (p + 4 > b.size())
        throw std::runtime_error("Bad test mutation");
    std::memcpy(b.data() + p, &v, 4);
}
unsigned Get(const xcx::Bytes &b, std::size_t p) {
    if (p + 4 > b.size())
        throw std::runtime_error("Bad test read");
    unsigned v;
    std::memcpy(&v, b.data() + p, 4);
    return v;
}
int main(int argc, char **argv) try {
    if (argc != 4 && !(argc == 5 && std::string_view(argv[4]) == "--pixels")) {
        std::cerr << "usage: xcx-model-probe MERGED-ROMFS OWNED-0x400-RECORD NEW-OUTPUT-DIRECTORY "
                     "[--pixels]\n";
        return 2;
    }
    fs::path romfs = fs::canonical(argv[1]), out = argv[3];
    if (fs::exists(out))
        throw std::runtime_error("Output already exists");
    std::size_t reads = 0;
    struct State {
        fs::path root;
        std::size_t *reads;
    } state{romfs, &reads};
    EdenDsmodHostApi host{};
    host.userdata = &state;
    host.read_romfs = [](void *u, const char *path, std::uint64_t offset, void *buffer,
                         std::size_t size) -> std::uint64_t {
        auto &s = *static_cast<State *>(u);
        ++*s.reads;
        fs::path rel = fs::path(path).relative_path();
        for (const auto &part : rel)
            if (part == "..")
                return 0;
        // This probe reads the two archive files only; never arbitrary model input paths.
        if (rel != "sts.arh" && rel != "sts.ard")
            return 0;
        std::ifstream f(s.root / rel, std::ios::binary);
        if (!f || offset > INT64_MAX || size > INT64_MAX)
            return 0;
        f.seekg(offset);
        f.read(static_cast<char *>(buffer), size);
        return f.gcount();
    };
    xcx::Archive archive;
    const auto record = Read(argv[2]);
    const auto before = reads;
    Refuses([&] { xcx::OwnedFrameModel(archive, host, {}); }, "Accepted missing record");
    auto bad = record;
    if (bad.size() != 0x400)
        throw std::runtime_error("Expected exact owned record");
    bad[0x3f4] = 255;
    Refuses([&] { xcx::OwnedFrameModel(archive, host, bad); }, "Accepted unknown record mode");
    if (reads != before)
        throw std::runtime_error("Invalid record accessed archive");
    const auto started = std::chrono::steady_clock::now();
    const auto frame = xcx::OwnedFrameModel(archive, host, record);
    const auto geometry = xcx::ReadFrameGeometry(archive, host, frame);
    const double prepare_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    bad = record;
    bad[0x22c] = bad[0x22d] = 255;
    Refuses([&] { xcx::OwnedFrameModel(archive, host, bad); }, "Accepted invalid frame row");
    auto bad_frame = frame;
    bad_frame.resource = "../wrong";
    const auto reads_before_path = reads;
    Refuses([&] { xcx::ReadFrameGeometry(archive, host, bad_frame); }, "Accepted unsafe resource");
    if (reads != reads_before_path)
        throw std::runtime_error("Unsafe resource accessed archive");
    const auto stem = "/chr/dl/" + frame.resource;
    const auto md = archive.Read(host, stem + ".wimdo"), mt = archive.Read(host, stem + ".wismt");
    Refuses([&] { xcx::DecodeFrameGeometry({}, mt); }, "Accepted absent model");
    Refuses([&] { xcx::DecodeFrameGeometry(md, {}); }, "Accepted absent stream");
    Refuses([&] { xcx::DecodeFrameGeometry(std::span(md).first(40), mt); },
            "Accepted truncated model header");
    Refuses([&] { xcx::DecodeFrameGeometry(md, std::span(mt).first(32)); },
            "Accepted truncated stream header");
    bad = md;
    Put(bad, 4, 10112);
    Refuses([&] { xcx::DecodeFrameGeometry(bad, mt); }, "Accepted unknown model schema");
    bad = md;
    Put(bad, Get(md, 8) + 28, UINT32_MAX);
    Refuses([&] { xcx::DecodeFrameGeometry(bad, mt); }, "Accepted excessive models");
    const auto models = Get(md, 8), first_model = models + Get(md, models + 24);
    bad = md;
    Put(bad, models + Get(md, first_model) + 8, UINT32_MAX);
    Refuses([&] { xcx::DecodeFrameGeometry(bad, mt); }, "Accepted missing mesh vertex buffer");
    bad = md;
    Put(bad, models + Get(md, first_model) + 20, UINT32_MAX);
    Refuses([&] { xcx::DecodeFrameGeometry(bad, mt); }, "Accepted missing mesh material");
    const auto materials = Get(md, 12), first_material = materials + Get(md, materials);
    bad = md;
    Put(bad, materials + Get(md, first_material + 60), UINT32_MAX);
    Refuses([&] { xcx::DecodeFrameGeometry(bad, mt); }, "Accepted missing material texture");
    bad = mt;
    const auto streaming = Get(mt, 12), high_textures = streaming + Get(mt, streaming + 72);
    Put(bad, high_textures, Get(mt, high_textures) ^ 1);
    Refuses([&] { xcx::DecodeFrameGeometry(md, bad); }, "Accepted mismatched shared texture");
    const auto low_textures = streaming + Get(mt, streaming + 60);
    bad = mt;
    Put(bad, low_textures + Get(mt, low_textures + 4) + 4, 0);
    Refuses([&] { xcx::DecodeFrameGeometry(md, bad); }, "Accepted absent low texture data");
    bad = mt;
    const auto streams = Get(mt, 12) + Get(mt, Get(mt, 12) + 20);
    const auto xbc = Get(mt, streams + 8);
    bad.at(xbc + 48 + Get(mt, xbc + 12) - 1) ^= 1;
    Refuses([&] { xcx::DecodeFrameGeometry(md, bad); }, "Accepted corrupt compressed geometry");
    xcx::Bytes packed(mt.begin() + xbc, mt.begin() + xbc + Get(mt, streams));
    auto bad_stream = packed;
    bad_stream.insert(bad_stream.end(), 16, 0);
    Refuses([&] { xcx::DecodeModelStream(bad_stream, Get(packed, 8)); },
            "Accepted extra stream tail");
    bad_stream = packed;
    const auto payload_end = 48 + Get(packed, 12);
    if (payload_end < packed.size()) {
        bad_stream.back() = 1;
        Refuses([&] { xcx::DecodeModelStream(bad_stream, Get(packed, 8)); },
                "Accepted nonzero padding");
    }
    bad_stream = packed;
    Put(bad_stream, 8, UINT32_MAX);
    Refuses([&] { xcx::DecodeModelStream(bad_stream, UINT32_MAX); }, "Accepted oversized stream");
    const auto &texture = geometry.texture_sources.front();
    xcx::Bytes low(geometry.low_texture_data.begin() + texture.low_offset,
                   geometry.low_texture_data.begin() + texture.low_offset + texture.low_size);
    const auto footer = low.size() - 40;
    Refuses([&] { xcx::DecodeModelTexture({}); }, "Accepted missing texture footer");
    Refuses([&] { xcx::DecodeModelTexture(low, {}, 0); }, "Accepted exhausted pixel budget");
    bad = low;
    Put(bad, footer + 24, UINT32_MAX);
    Refuses([&] { xcx::DecodeModelTexture(bad); }, "Accepted unsupported texture format");
    bad = low;
    Put(bad, footer + 24, 109);
    Refuses([&] { xcx::DecodeModelTexture(bad); }, "Accepted unverified BGRA texture");
    bad = low;
    Put(bad, footer + 16, 2);
    Refuses([&] { xcx::DecodeModelTexture(bad); }, "Accepted volume texture");
    bad = low;
    Put(bad, footer + 8, UINT32_MAX);
    Refuses([&] { xcx::DecodeModelTexture(bad); }, "Accepted excessive image dimensions");
    xcx::Bytes sample(4096, 0);
    const auto sf = sample.size() - 40;
    Put(sample, sf, 4096);
    Put(sample, sf + 4, 4096);
    Put(sample, sf + 8, 4);
    Put(sample, sf + 12, 4);
    Put(sample, sf + 16, 1);
    Put(sample, sf + 20, 1);
    Put(sample, sf + 24, 75);
    Put(sample, sf + 28, 1);
    Put(sample, sf + 32, 10001);
    Put(sample, sf + 36, 0x4d49424c);
    sample[0] = 255;
    sample[9] = 255;
    std::uint64_t packed_indices = 0;
    for (unsigned n = 0; n < 16; ++n)
        packed_indices |= std::uint64_t(2) << (n * 3);
    for (unsigned n = 0; n < 6; ++n)
        sample[2 + n] = sample[10 + n] = packed_indices >> (n * 8);
    const auto decoded_sample = xcx::DecodeModelTexture(sample);
    for (unsigned n = 0; n < 16; ++n)
        if (decoded_sample.rgba[n * 4] != 219 || decoded_sample.rgba[n * 4 + 1] != 51 ||
            decoded_sample.rgba[n * 4 + 2] != 0 || decoded_sample.rgba[n * 4 + 3] != 255)
            throw std::runtime_error("BC5 quantization/channel check failed");
    xcx::FrameGeometry unsafe_texture;
    unsafe_texture.texture_sources.push_back({"00000000", "../wrong", "", 0, 0, 0});
    xcx::FrameTextureReader unsafe_reader;
    const auto before_unsafe = reads;
    Refuses([&] { unsafe_reader.Read(host, unsafe_texture, 0); }, "Accepted unsafe shared path");
    if (reads != before_unsafe)
        throw std::runtime_error("Unsafe texture read archive");
    Json j{
        {"scope", "native frame bind-geometry preparation only; not current equipped appearance"},
        {"owned_record_input", fs::absolute(argv[2]).string()},
        {"frame_row", frame.frame},
        {"name_id", frame.name_id},
        {"resource", frame.resource},
        {"archive_paths", {"/bdat/common.bdat", stem + ".wimdo", stem + ".wismt"}},
        {"model_schema", "DMXM10040 + DRSM10001/4097 + legacy vertex"},
        {"unavailable",
         {"native paint binding", "pose/skinning", "equipped attachments",
          "shader/texture pixels"}},
        {"prepare_ms", prepare_ms},
        {"refusal_checks_passed", true}};
    j["vertex_buffers"] = Json::array();
    for (const auto &buffer : geometry.vertices) {
        Json vertices = Json::array();
        for (const auto &v : buffer)
            vertices.push_back({{"position", v.position},
                                {"normal", v.normal},
                                {"tangent", v.tangent},
                                {"uv", v.uv}});
        j["vertex_buffers"].push_back(std::move(vertices));
    }
    j["index_buffers"] = geometry.indices;
    j["materials"] = Json::array();
    for (const auto &m : geometry.materials) {
        Json bindings = Json::array();
        for (const auto &t : m.textures)
            bindings.push_back({{"texture", t.texture}, {"sampler_flags", t.sampler_flags}});
        j["materials"].push_back({{"name", m.name}, {"color", m.color}, {"textures", bindings}});
    }
    j["texture_sources"] = Json::array();
    for (const auto &t : geometry.texture_sources)
        j["texture_sources"].push_back({{"name", t.name},
                                        {"usage", t.usage},
                                        {"low_offset", t.low_offset},
                                        {"low_size", t.low_size},
                                        {"mip_path", t.mip_path},
                                        {"base_mip_path", t.base_mip_path}});
    j["meshes"] = Json::array();
    std::size_t triangles = 0;
    for (const auto &m : geometry.meshes) {
        j["meshes"].push_back({{"vertex_buffer", m.vertex_buffer},
                               {"index_buffer", m.index_buffer},
                               {"material", m.material},
                               {"flags", m.flags}});
        triangles += geometry.indices[m.index_buffer].size() / 3;
    }
    j["triangle_count"] = triangles;
    fs::create_directories(out);
    if (argc == 5) {
        fs::create_directory(out / "textures");
        xcx::FrameTextureReader textures;
        j["texture_images"] = Json::array();
        std::uint64_t total = 0, largest = 0;
        double decode_ms = 0;
        unsigned largest_index = 0;
        for (unsigned n = 0; n < geometry.texture_sources.size(); ++n) {
            const auto decode_start = std::chrono::steady_clock::now();
            const auto image = textures.Read(host, geometry, n);
            const auto elapsed = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - decode_start)
                                     .count();
            decode_ms += elapsed;
            const auto name = std::to_string(n) + "-" + geometry.texture_sources[n].name + ".rgba";
            std::ofstream pixels(out / "textures" / name, std::ios::binary);
            pixels.write(reinterpret_cast<const char *>(image.rgba.data()), image.rgba.size());
            if (!pixels)
                throw std::runtime_error("Cannot write decoded texture");
            const auto count = std::uint64_t(image.w) * image.h;
            total += count;
            if (count > largest) {
                largest = count;
                largest_index = n;
            }
            j["texture_images"].push_back({{"index", n},
                                           {"width", image.w},
                                           {"height", image.h},
                                           {"file", "textures/" + name},
                                           {"decode_ms", elapsed}});
        }
        // One batch owns only one decoded image at a time; exercise the cumulative cap.
        bool exhausted = false;
        for (unsigned n = 0; n < 32; ++n) {
            try {
                textures.Read(host, geometry, largest_index);
            } catch (const std::runtime_error &e) {
                if (std::string_view(e.what()) != "XCX model texture pixel budget")
                    throw;
                exhausted = true;
                break;
            }
        }
        if (!exhausted)
            throw std::runtime_error("Texture batch cap was not enforced");
        j["texture_texels"] = total;
        j["texture_decode_ms_excluding_output"] = decode_ms;
        j["texture_batch_budget_verified"] = true;
        j["texture_scope"] = "top mip/layer0 RGBA only; no shader/paint/pose/equipment application";
        j["unavailable"] = {"native paint binding", "pose/skinning", "equipped attachments",
                            "native shader"};
    }
    std::ofstream f(out / "geometry.json");
    f << j.dump() << '\n';
    if (!f)
        throw std::runtime_error("Cannot write model diagnostic");
    std::cout << frame.resource << " frame=" << frame.frame << " meshes=" << geometry.meshes.size()
              << " triangles=" << triangles << " prepare_ms=" << prepare_ms
              << "; native format/refusal checks passed\n";
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
